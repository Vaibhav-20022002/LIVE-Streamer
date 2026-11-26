#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

#include "capture/av_raii.hpp"
#include "capture/backoff.hpp"
#include "capture/source_capture.hpp"
#include "common/config.hpp"
#include "common/logger.hpp"
#include "inference/ai_worker.hpp"
#include "inference/yuv_overlay.hpp"
#include "muxer/hls_muxer.hpp"
#include "muxer/playback_manager.hpp"
#include "pipeline/profiler.hpp"
#include "pipeline/stats_writer.hpp"

namespace {

std::atomic<bool> g_stop_requested{false};

extern "C" void handle_signal(int) {
  g_stop_requested.store(true, std::memory_order_relaxed);
}

struct TranscodeResources {
  streamer::AVCodecCtxPtr dec_ctx;
  streamer::AVFramePtr    dec_frame;
  streamer::AVFramePtr    yuv_frame;
  streamer::AVFramePtr    rgb_frame;
  streamer::SwsCtxPtr     sws_yuv;
  streamer::SwsCtxPtr     sws_rgb;
};

void cleanup_stale_hls_files(const std::filesystem::path &dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return;
  }
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    const std::string ext  = entry.path().extension().string();
    if (ext == ".m4s" || ext == ".m3u8" || ext == ".tmp" ||
        (name.rfind("init_", 0) == 0 && ext == ".mp4") || name == "stats.json" ||
        name == "events.json") {
      std::filesystem::remove(entry.path(), ec);
    }
  }
}

bool deadline_reached(int64_t app_start_ms, int max_duration_sec) {
  return max_duration_sec > 0 &&
      (streamer::now_steady_ms() - app_start_ms) >= static_cast<int64_t>(max_duration_sec) * 1000LL;
}

bool sleep_interruptible_ms(int total_ms, int64_t app_start_ms, int max_duration_sec) {
  int remaining = total_ms;
  while (remaining > 0 && !g_stop_requested.load(std::memory_order_relaxed)) {
    if (deadline_reached(app_start_ms, max_duration_sec)) {
      return false;
    }
    const int chunk = std::min(remaining, 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
    remaining -= chunk;
  }
  return !g_stop_requested.load(std::memory_order_relaxed);
}

bool choose_remux_mode(const streamer::Config &cfg, const streamer::SourceCapture &capture) {
  const bool can_remux = capture.can_remux();
  if (cfg.transcode_mode == streamer::TranscodeMode::Remux) {
    if (can_remux) {
      return true;
    }
    streamer::log_warn("pipeline",
        "TRANSCODE_MODE=remux requested, but input "
        "codec/source requires transcode");
    return false;
  }
  if (cfg.transcode_mode == streamer::TranscodeMode::Auto) {
    return (!cfg.ai_enabled && can_remux);
  }
  return false;
}

bool init_transcode_resources(const streamer::Config &cfg,
    const streamer::SourceCapture                    &capture,
    int                                               out_w,
    int                                               out_h,
    bool                                              need_rgb,
    TranscodeResources                               &res,
    std::string                                      &err) {
  const AVCodec *dec = avcodec_find_decoder(capture.codec_id());
  if (!dec) {
    err = "Decoder not found for codec id " + std::to_string(capture.codec_id());
    return false;
  }

  res.dec_ctx.reset(avcodec_alloc_context3(dec));
  if (!res.dec_ctx ||
      avcodec_parameters_to_context(res.dec_ctx.get(), capture.video_stream()->codecpar) < 0) {
    err = "Failed to copy decoder parameters";
    return false;
  }

  /// Cap decoder threads to avoid oversubscription with libx264/ONNX.
  res.dec_ctx->thread_count = std::max(1, std::min(2, cfg.encoder_threads));
  if (avcodec_open2(res.dec_ctx.get(), dec, nullptr) < 0) {
    err = "Failed to open decoder";
    return false;
  }

  res.dec_frame.reset(av_frame_alloc());
  res.yuv_frame.reset(av_frame_alloc());
  if (!res.dec_frame || !res.yuv_frame) {
    err = "Failed to allocate video frames";
    return false;
  }

  res.yuv_frame->format = AV_PIX_FMT_YUV420P;
  res.yuv_frame->width  = out_w;
  res.yuv_frame->height = out_h;
  if (av_frame_get_buffer(res.yuv_frame.get(), 32) < 0) {
    err = "Failed to allocate YUV420P frame buffer";
    return false;
  }

  if (need_rgb) {
    res.rgb_frame.reset(av_frame_alloc());
    if (!res.rgb_frame) {
      err = "Failed to allocate RGB frame";
      return false;
    }
    res.rgb_frame->format = AV_PIX_FMT_RGB24;
    res.rgb_frame->width  = out_w;
    res.rgb_frame->height = out_h;
    if (av_frame_get_buffer(res.rgb_frame.get(), 32) < 0) {
      err = "Failed to allocate RGB24 frame buffer";
      return false;
    }
  }
  return true;
}

std::string format_telemetry_line(const std::string &mode,
    double                                           fps,
    uint64_t                                         frames_processed,
    uint64_t                                         dropped_ai,
    const streamer::AiWorker                        &ai_worker,
    const streamer::PlaybackManager                 &playback_mgr,
    const streamer::StageProfiler                   &profiler,
    uint64_t                                         reconnect_count) {
  const auto         ai_stats  = profiler.snapshot(streamer::Stage::Inference);
  const auto         enc_stats = profiler.snapshot(streamer::Stage::Encode);
  const auto         tot_stats = profiler.snapshot(streamer::Stage::TotalFrame);
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2) << "mode=" << mode << " fps=" << fps
      << " frames=" << frames_processed << " ai_dropped=" << dropped_ai
      << " ai_inf=" << ai_worker.inference_count() << " ai_ms=" << ai_stats.avg_ms << "/"
      << ai_stats.p95_ms << " enc_ms=" << enc_stats.avg_ms << "/" << enc_stats.p95_ms
      << " total_ms=" << tot_stats.avg_ms << "/" << tot_stats.p95_ms
      << " dvr_segs=" << playback_mgr.playback_count() << " dvr_sec=" << std::setprecision(1)
      << playback_mgr.total_duration_sec() << " reconnects=" << reconnect_count;
  return oss.str();
}

} //< namespace

int main(int argc, char **argv) {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  /// Keep AV_LOG_WARNING+ to silence Libav HLS segment INFO chatter.
  av_log_set_level(AV_LOG_WARNING);

  const std::string env_path = (argc > 1 && argv[1]) ? argv[1] : ".env";
  streamer::Config  cfg;
  try {
    cfg = streamer::load_config(env_path);
  } catch (const std::exception &ex) {
    streamer::log_error("config", std::string("Configuration error: ") + ex.what());
    return 1;
  }

  {
    std::ostringstream banner;
    banner << "Starting Live Video Streamer | source="
           << streamer::source_type_to_string(cfg.source_type) << " url=" << cfg.source_url
           << " transcode=" << streamer::transcode_mode_to_string(cfg.transcode_mode)
           << " hls_dir=" << cfg.hls_output_dir << " seg=" << cfg.hls_segment_duration << "s"
           << " retention=" << cfg.playback_retention_sec << "s"
           << " ai=" << (cfg.ai_enabled ? "true" : "false")
           << " sync=" << (cfg.ai_sync_mode ? "true" : "false")
           << " pool=" << (cfg.use_buffer_pool ? "true" : "false");
    streamer::log_info("streamer", banner.str());
  }

  const std::filesystem::path hls_dir(cfg.hls_output_dir);
  cleanup_stale_hls_files(hls_dir);

  streamer::StageProfiler   profiler;
  streamer::PlaybackManager playback_mgr(hls_dir, cfg.playback_retention_sec);
  streamer::AiWorker        ai_worker(cfg, profiler);
  ai_worker.start();

  streamer::ExponentialBackoff backoff(cfg.reconnect_base_ms, cfg.reconnect_max_ms);
  const int64_t                app_start_ms = streamer::now_steady_ms();

  int         reconnect_epoch  = 0;
  uint64_t    reconnect_count  = 0;
  uint64_t    frames_processed = 0;
  int         last_width       = cfg.source_width;
  int         last_height      = cfg.source_height;
  std::string last_mode        = "transcode";
  double      last_fps_in      = 0.0;
  double      last_fps_out     = 0.0;

  auto publish_stats = [&](const std::string &state,
                           const std::string &mode,
                           int                w,
                           int                h,
                           double             fps_in,
                           double             fps_out) {
    streamer::PipelineMetrics m;
    m.state                 = state;
    m.mode                  = mode;
    m.source_type           = streamer::source_type_to_string(cfg.source_type);
    m.width                 = w;
    m.height                = h;
    m.fps_in                = fps_in;
    m.fps_out               = fps_out;
    m.frames_processed      = frames_processed;
    m.frames_dropped        = ai_worker.dropped_jobs();
    m.reconnect_count       = reconnect_count;
    m.active_live_segments  = playback_mgr.active_live_count();
    m.playback_segments     = playback_mgr.playback_count();
    m.playback_duration_sec = playback_mgr.total_duration_sec();
    m.ai_enabled            = ai_worker.is_active();
    m.ai_backend            = ai_worker.backend_name();
    m.ai_inferences         = ai_worker.inference_count();
    m.latest_detections     = ai_worker.latest_detections();
    m.decode_stats          = profiler.snapshot(streamer::Stage::Decode);
    m.inference_stats       = profiler.snapshot(streamer::Stage::Inference);
    m.encode_stats          = profiler.snapshot(streamer::Stage::Encode);
    m.mux_stats             = profiler.snapshot(streamer::Stage::Mux);
    m.total_stats           = profiler.snapshot(streamer::Stage::TotalFrame);
    m.uptime_sec         = static_cast<double>(streamer::now_steady_ms() - app_start_ms) / 1000.0;
    m.peak_rss_mb        = streamer::peak_rss_mb();
    m.timestamp_epoch_ms = streamer::now_epoch_ms();
    streamer::write_stats_json(hls_dir, m);
  };

  while (!g_stop_requested.load(std::memory_order_relaxed) &&
      !deadline_reached(app_start_ms, cfg.max_duration_sec)) {
    publish_stats(reconnect_epoch == 0 ? "connecting" : "reconnecting",
        last_mode,
        last_width,
        last_height,
        0.0,
        0.0);

    streamer::SourceCapture capture(cfg, g_stop_requested);
    std::string             err;
    if (!capture.open(err)) {
      streamer::log_warn("capture", "Open failed: " + err);
      ++reconnect_count;
      const int delay = backoff.next_delay_ms();
      streamer::log_info("resilience", "Retrying capture in " + std::to_string(delay) + " ms");
      if (!sleep_interruptible_ms(delay, app_start_ms, cfg.max_duration_sec)) {
        break;
      }
      continue;
    }

    backoff.reset();

    const int out_w   = (capture.width() > 0) ? capture.width() : cfg.source_width;
    const int out_h   = (capture.height() > 0) ? capture.height() : cfg.source_height;
    const int out_fps = (capture.fps() > 0) ? capture.fps() : cfg.source_fps;
    last_width        = out_w;
    last_height       = out_h;

    const bool use_remux      = choose_remux_mode(cfg, capture);
    last_mode                 = use_remux ? "remux" : "transcode";
    const int  start_seg      = playback_mgr.next_segment_number();
    const bool append_discont = (reconnect_epoch > 0);

    streamer::HlsMuxer muxer(cfg);
    const bool         mux_ok = use_remux
        ? muxer.open_remux(capture.video_stream(), start_seg, reconnect_epoch, append_discont, err)
        : muxer.open_transcode(
              out_w, out_h, out_fps, start_seg, reconnect_epoch, append_discont, err);
    if (!mux_ok) {
      streamer::log_error("muxer", "Open failed: " + err);
      capture.close();
      ++reconnect_count;
      const int delay = backoff.next_delay_ms();
      if (!sleep_interruptible_ms(delay, app_start_ms, cfg.max_duration_sec)) {
        break;
      }
      continue;
    }

    TranscodeResources transcode;
    if (!use_remux &&
        !init_transcode_resources(
            cfg, capture, out_w, out_h, ai_worker.is_active(), transcode, err)) {
      streamer::log_error("decode", err);
      muxer.close();
      capture.close();
      ++reconnect_count;
      const int delay = backoff.next_delay_ms();
      if (!sleep_interruptible_ms(delay, app_start_ms, cfg.max_duration_sec)) {
        break;
      }
      continue;
    }

    {
      std::ostringstream ss;
      ss << "Streaming active | epoch=" << reconnect_epoch << " mode=" << last_mode << " " << out_w
         << "x" << out_h << "@" << out_fps << "fps start_seg=" << start_seg
         << " init=" << muxer.init_filename();
      streamer::log_info("pipeline", ss.str());
    }

    streamer::AVPacketPtr pkt(av_packet_alloc());
    int64_t               fps_window_start_ms = streamer::now_steady_ms();
    uint64_t              fps_window_frames   = 0;
    int64_t               last_sync_ms        = fps_window_start_ms;
    int64_t               last_stats_ms       = fps_window_start_ms;
    int64_t               last_prof_ms        = fps_window_start_ms;

    while (!g_stop_requested.load(std::memory_order_relaxed) &&
        !deadline_reached(app_start_ms, cfg.max_duration_sec)) {
      av_packet_unref(pkt.get());

      const auto           cap_t0 = std::chrono::steady_clock::now();
      streamer::ReadStatus rs     = capture.read_packet(pkt.get(), err);
      const auto           cap_t1 = std::chrono::steady_clock::now();
      profiler.record(streamer::Stage::Capture,
          std::chrono::duration<double, std::milli>(cap_t1 - cap_t0).count());

      if (rs == streamer::ReadStatus::Stopped) {
        break;
      }
      if (rs == streamer::ReadStatus::Eof || rs == streamer::ReadStatus::Timeout ||
          rs == streamer::ReadStatus::Error) {
        streamer::log_warn("capture", "Stream interrupted (" + err + ")");
        break;
      }

      const auto frame_t0 = std::chrono::steady_clock::now();

      if (use_remux) {
        {
          streamer::StageProfiler::ScopedTimer t_mux(profiler, streamer::Stage::Mux);
          if (!muxer.write_remux_packet(pkt.get(), capture.video_stream()->time_base, err)) {
            streamer::log_error("muxer", "Remux packet failed: " + err);
            break;
          }
        }
        const auto frame_t1 = std::chrono::steady_clock::now();
        profiler.record(streamer::Stage::TotalFrame,
            std::chrono::duration<double, std::milli>(frame_t1 - frame_t0).count());
        ++frames_processed;
        ++fps_window_frames;
      } else {
        int send_ret = 0;
        {
          streamer::StageProfiler::ScopedTimer t_dec(profiler, streamer::Stage::Decode);
          send_ret = avcodec_send_packet(transcode.dec_ctx.get(), pkt.get());
        }
        if (send_ret < 0) {
          continue;
        }

        bool mux_failed = false;
        while (true) {
          int recv_ret = 0;
          {
            streamer::StageProfiler::ScopedTimer t_dec(profiler, streamer::Stage::Decode);
            recv_ret = avcodec_receive_frame(transcode.dec_ctx.get(), transcode.dec_frame.get());
          }
          if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF || recv_ret < 0) {
            break;
          }

          if (!transcode.sws_yuv) {
            const AVPixelFormat src_fmt = static_cast<AVPixelFormat>(transcode.dec_frame->format);
            transcode.sws_yuv.reset(sws_getContext(transcode.dec_frame->width,
                transcode.dec_frame->height,
                src_fmt,
                out_w,
                out_h,
                AV_PIX_FMT_YUV420P,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr));
            if (ai_worker.is_active()) {
              transcode.sws_rgb.reset(sws_getContext(transcode.dec_frame->width,
                  transcode.dec_frame->height,
                  src_fmt,
                  out_w,
                  out_h,
                  AV_PIX_FMT_RGB24,
                  SWS_FAST_BILINEAR,
                  nullptr,
                  nullptr,
                  nullptr));
            }
          }

          av_frame_make_writable(transcode.yuv_frame.get());
          sws_scale(transcode.sws_yuv.get(),
              transcode.dec_frame->data,
              transcode.dec_frame->linesize,
              0,
              transcode.dec_frame->height,
              transcode.yuv_frame->data,
              transcode.yuv_frame->linesize);

          if (ai_worker.is_active() && transcode.sws_rgb) {
            av_frame_make_writable(transcode.rgb_frame.get());
            sws_scale(transcode.sws_rgb.get(),
                transcode.dec_frame->data,
                transcode.dec_frame->linesize,
                0,
                transcode.dec_frame->height,
                transcode.rgb_frame->data,
                transcode.rgb_frame->linesize);

            std::vector<streamer::Detection> dets;
            if (cfg.ai_sync_mode) {
              dets = ai_worker.infer_sync(
                  transcode.rgb_frame->data[0], transcode.rgb_frame->linesize[0], out_w, out_h);
            } else {
              ai_worker.submit_rgb_frame(
                  transcode.rgb_frame->data[0], transcode.rgb_frame->linesize[0], out_w, out_h);
              dets = ai_worker.latest_detections();
            }

            if (!dets.empty()) {
              {
                streamer::StageProfiler::ScopedTimer t_ov(profiler, streamer::Stage::Overlay);
                streamer::draw_detections_yuv420p(transcode.yuv_frame->data[0],
                    transcode.yuv_frame->linesize[0],
                    transcode.yuv_frame->data[1],
                    transcode.yuv_frame->linesize[1],
                    transcode.yuv_frame->data[2],
                    transcode.yuv_frame->linesize[2],
                    out_w,
                    out_h,
                    dets);
              }
              playback_mgr.record_detections(streamer::now_epoch_ms(), dets);
            }
          }

          {
            streamer::StageProfiler::ScopedTimer t_enc(profiler, streamer::Stage::Encode);
            if (!muxer.encode_and_write_frame(transcode.yuv_frame.get(), err)) {
              streamer::log_error("muxer", "Encode/write failed: " + err);
              mux_failed = true;
              break;
            }
          }

          const auto frame_t1 = std::chrono::steady_clock::now();
          profiler.record(streamer::Stage::TotalFrame,
              std::chrono::duration<double, std::milli>(frame_t1 - frame_t0).count());
          ++frames_processed;
          ++fps_window_frames;
          av_frame_unref(transcode.dec_frame.get());
        }
        if (mux_failed) {
          break;
        }
      }

      const int64_t now_ms = streamer::now_steady_ms();
      if (now_ms - last_sync_ms >= 500) {
        playback_mgr.sync_from_live_playlist();
        playback_mgr.prune_expired();
        playback_mgr.write_playback_manifest();
        playback_mgr.write_events_json();
        last_sync_ms = now_ms;
      }

      if (now_ms - last_stats_ms >= 1000) {
        const double elapsed_sec = static_cast<double>(now_ms - fps_window_start_ms) / 1000.0;
        if (elapsed_sec > 0.05) {
          const double measured_fps = static_cast<double>(fps_window_frames) / elapsed_sec;
          last_fps_in               = measured_fps;
          last_fps_out              = measured_fps;
        }
        fps_window_start_ms = now_ms;
        fps_window_frames   = 0;
        publish_stats("streaming", last_mode, out_w, out_h, last_fps_in, last_fps_out);
        last_stats_ms = now_ms;
      }

      if (now_ms - last_prof_ms >= static_cast<int64_t>(cfg.profiling_log_interval_sec) * 1000LL) {
        streamer::log_info("telemetry",
            format_telemetry_line(last_mode,
                last_fps_out,
                frames_processed,
                ai_worker.dropped_jobs(),
                ai_worker,
                playback_mgr,
                profiler,
                reconnect_count));
        last_prof_ms = now_ms;
      }
    }

    muxer.close();
    capture.close();
    playback_mgr.sync_from_live_playlist();
    playback_mgr.prune_expired();
    playback_mgr.write_playback_manifest();
    playback_mgr.write_events_json();

    if (g_stop_requested.load(std::memory_order_relaxed) ||
        deadline_reached(app_start_ms, cfg.max_duration_sec)) {
      break;
    }

    ++reconnect_epoch;
    ++reconnect_count;
    playback_mgr.mark_discontinuity("init_" + std::to_string(reconnect_epoch) + ".mp4");
    publish_stats("reconnecting", last_mode, out_w, out_h, 0.0, 0.0);

    const int delay = backoff.next_delay_ms();
    streamer::log_warn("resilience",
        "Reconnecting (epoch=" + std::to_string(reconnect_epoch) + ") after " +
            std::to_string(delay) + " ms backoff");
    if (!sleep_interruptible_ms(delay, app_start_ms, cfg.max_duration_sec)) {
      break;
    }
  }

  ai_worker.stop();
  playback_mgr.sync_from_live_playlist();
  playback_mgr.prune_expired();
  playback_mgr.write_playback_manifest();
  playback_mgr.write_events_json();
  publish_stats("stopped", last_mode, last_width, last_height, last_fps_in, last_fps_out);

  {
    std::ostringstream ss;
    ss << "Shutdown complete | frames=" << frames_processed << " reconnects=" << reconnect_count
       << " ai_inferences=" << ai_worker.inference_count() << "\n"
       << profiler.format_summary();
    streamer::log_info("streamer", ss.str());
  }
  return 0;
}
