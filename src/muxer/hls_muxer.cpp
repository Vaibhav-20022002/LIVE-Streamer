#include "muxer/hls_muxer.hpp"

extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <filesystem>
#include <system_error>

/// @file hls_muxer.cpp
/// @brief Implements FFmpeg fMP4 (`hls_segment_type=fmp4`) muxer
/// initialization, shared-segment
///        live window (`live.m3u8`), and DTS monotonicity across stream
///        reconnects.
///
/// Architecture & Invariants:
/// - fMP4 Muxing: Writes initialization segment (`init_epoch.mp4`) and `.m4s`
/// media fragments with
///   `hls_flags=independent_segments+discont_start+temp_file`.
/// - DTS Monotonicity: Enforces strict monotonically increasing DTS/PTS across
/// stream reconnects
///   and discontinuous timestamps to prevent player stalled buffering or demux
///   errors.

namespace streamer {

namespace {

void populate_hls_options(AVDictionary **opts,
    const Config                        &cfg,
    int                                  start_segment_num,
    const std::string                   &init_filename,
    bool                                 append_discontinuity) {
  const std::string seg_pattern =
      (std::filesystem::path(cfg.hls_output_dir) / "seg_%06d.m4s").string();
  const std::string seg_duration = std::to_string(std::max(1, cfg.hls_segment_duration));
  const std::string list_size    = std::to_string(std::max(1, cfg.hls_live_list_size));
  const std::string start_num    = std::to_string(std::max(0, start_segment_num));
  const char       *flags        = append_discontinuity
      ? "independent_segments+discont_start+append_list+temp_file"
      : "independent_segments+discont_start+temp_file";

  av_dict_set(opts, "hls_segment_type", "fmp4", 0);
  av_dict_set(opts, "hls_fmp4_init_filename", init_filename.c_str(), 0);
  av_dict_set(opts, "hls_time", seg_duration.c_str(), 0);
  av_dict_set(opts, "hls_list_size", list_size.c_str(), 0);
  av_dict_set(opts, "hls_segment_filename", seg_pattern.c_str(), 0);
  av_dict_set(opts, "start_number", start_num.c_str(), 0);
  av_dict_set(opts, "hls_flags", flags, 0);
}

} //< namespace

HlsMuxer::HlsMuxer(const Config &cfg)
    : cfg_(cfg) {}

HlsMuxer::~HlsMuxer() {
  close();
}

bool HlsMuxer::init_hls_output(int init_epoch, std::string &err) {
  std::error_code ec;
  std::filesystem::create_directories(cfg_.hls_output_dir, ec);
  if (ec) {
    err = "Failed to create HLS output directory '" + cfg_.hls_output_dir + "': " + ec.message();
    return false;
  }

  const std::string out_path = (std::filesystem::path(cfg_.hls_output_dir) / "live.m3u8").string();

  AVFormatContext *raw_out = nullptr;
  const int        ret = avformat_alloc_output_context2(&raw_out, nullptr, "hls", out_path.c_str());
  if (ret < 0 || !raw_out) {
    err = "avformat_alloc_output_context2(hls) failed: " + av_err_str(ret);
    return false;
  }

  fmt_ctx_.reset(raw_out);
  /// Versioned init filename prevents stale MP4 boxes after camera reconnects.
  init_filename_ = "init_" + std::to_string(init_epoch) + ".mp4";
  return true;
}

bool HlsMuxer::open_remux(const AVStream *in_stream,
    int                                   start_segment_num,
    int                                   init_epoch,
    bool                                  append_discontinuity,
    std::string                          &err) {
  if (!in_stream || !in_stream->codecpar) {
    err = "Invalid input stream for HLS remux";
    return false;
  }

  close();
  remux_mode_   = true;
  first_in_pts_ = AV_NOPTS_VALUE;
  first_in_dts_ = AV_NOPTS_VALUE;
  last_dts_     = AV_NOPTS_VALUE;
  saw_keyframe_ = false;

  if (!init_hls_output(init_epoch, err)) {
    return false;
  }

  out_stream_ = avformat_new_stream(fmt_ctx_.get(), nullptr);
  if (!out_stream_) {
    err = "avformat_new_stream failed";
    fmt_ctx_.reset();
    return false;
  }

  int ret = avcodec_parameters_copy(out_stream_->codecpar, in_stream->codecpar);
  if (ret < 0) {
    err = "avcodec_parameters_copy failed: " + av_err_str(ret);
    fmt_ctx_.reset();
    out_stream_ = nullptr;
    return false;
  }
  /// Clear source container tag so fMP4 muxer writes ISO BMFF-compatible codec
  /// tags.
  out_stream_->codecpar->codec_tag = 0;
  out_stream_->time_base           = in_stream->time_base;

  AVDictionary *raw_opts = nullptr;
  populate_hls_options(&raw_opts, cfg_, start_segment_num, init_filename_, append_discontinuity);
  ret = avformat_write_header(fmt_ctx_.get(), &raw_opts);
  AVDictionaryPtr opts_cleanup(raw_opts);
  if (ret < 0) {
    err = "avformat_write_header(remux) failed: " + av_err_str(ret);
    fmt_ctx_.reset();
    out_stream_ = nullptr;
    return false;
  }

  enc_pkt_.reset(av_packet_alloc());
  if (!enc_pkt_) {
    err = "av_packet_alloc failed";
    av_write_trailer(fmt_ctx_.get());
    fmt_ctx_.reset();
    out_stream_ = nullptr;
    return false;
  }

  return true;
}

bool HlsMuxer::open_transcode(int width,
    int                           height,
    int                           fps,
    int                           start_segment_num,
    int                           init_epoch,
    bool                          append_discontinuity,
    std::string                  &err) {
  if (width <= 0 || height <= 0) {
    err = "Invalid frame dimensions for HLS transcode";
    return false;
  }

  close();
  remux_mode_   = false;
  next_pts_     = 0;
  last_dts_     = AV_NOPTS_VALUE;
  first_in_pts_ = AV_NOPTS_VALUE;
  first_in_dts_ = AV_NOPTS_VALUE;
  saw_keyframe_ = false;

  if (!init_hls_output(init_epoch, err)) {
    return false;
  }

  const AVCodec *encoder = avcodec_find_encoder_by_name("libx264");
  if (!encoder) {
    encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
  }
  if (!encoder) {
    err = "No H.264 encoder available (tried libx264 and AV_CODEC_ID_H264)";
    fmt_ctx_.reset();
    return false;
  }

  enc_ctx_.reset(avcodec_alloc_context3(encoder));
  if (!enc_ctx_) {
    err = "avcodec_alloc_context3 failed";
    fmt_ctx_.reset();
    return false;
  }

  const int safe_fps = std::max(1, fps);
  const int gop      = safe_fps * std::max(1, cfg_.hls_segment_duration);

  enc_ctx_->width        = width;
  enc_ctx_->height       = height;
  enc_ctx_->pix_fmt      = AV_PIX_FMT_YUV420P;
  enc_ctx_->time_base    = AVRational{1, safe_fps};
  enc_ctx_->framerate    = AVRational{safe_fps, 1};
  enc_ctx_->gop_size     = gop;
  enc_ctx_->max_b_frames = 0;
  enc_ctx_->bit_rate     = static_cast<int64_t>(cfg_.encoder_bitrate_kbps) * 1000;
  if (cfg_.encoder_threads > 0) {
    enc_ctx_->thread_count = cfg_.encoder_threads;
  }
  /// Out-of-band SPS/PPS extradata is required to populate fMP4 init.mp4 moov
  /// boxes.
  enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  AVDictionary *raw_enc_opts = nullptr;
  if (encoder->name && std::string(encoder->name) == "libx264") {
    av_dict_set(&raw_enc_opts, "preset", cfg_.encoder_preset.c_str(), 0);
    av_dict_set(&raw_enc_opts, "tune", "zerolatency", 0);
    const std::string x264_params =
        "keyint=" + std::to_string(gop) + ":min-keyint=" + std::to_string(gop) + ":scenecut=0";
    av_dict_set(&raw_enc_opts, "x264-params", x264_params.c_str(), 0);
  }

  int             ret = avcodec_open2(enc_ctx_.get(), encoder, &raw_enc_opts);
  AVDictionaryPtr enc_opts_cleanup(raw_enc_opts);
  if (ret < 0) {
    err = "avcodec_open2 failed: " + av_err_str(ret);
    enc_ctx_.reset();
    fmt_ctx_.reset();
    return false;
  }

  out_stream_ = avformat_new_stream(fmt_ctx_.get(), nullptr);
  if (!out_stream_) {
    err = "avformat_new_stream failed";
    enc_ctx_.reset();
    fmt_ctx_.reset();
    return false;
  }

  ret = avcodec_parameters_from_context(out_stream_->codecpar, enc_ctx_.get());
  if (ret < 0) {
    err = "avcodec_parameters_from_context failed: " + av_err_str(ret);
    enc_ctx_.reset();
    fmt_ctx_.reset();
    out_stream_ = nullptr;
    return false;
  }
  out_stream_->time_base = enc_ctx_->time_base;

  AVDictionary *raw_hls_opts = nullptr;
  populate_hls_options(
      &raw_hls_opts, cfg_, start_segment_num, init_filename_, append_discontinuity);
  ret = avformat_write_header(fmt_ctx_.get(), &raw_hls_opts);
  AVDictionaryPtr hls_opts_cleanup(raw_hls_opts);
  if (ret < 0) {
    err = "avformat_write_header(transcode) failed: " + av_err_str(ret);
    enc_ctx_.reset();
    fmt_ctx_.reset();
    out_stream_ = nullptr;
    return false;
  }

  enc_pkt_.reset(av_packet_alloc());
  if (!enc_pkt_) {
    err = "av_packet_alloc failed";
    av_write_trailer(fmt_ctx_.get());
    enc_ctx_.reset();
    fmt_ctx_.reset();
    out_stream_ = nullptr;
    return false;
  }

  return true;
}

bool HlsMuxer::write_remux_packet(AVPacket *pkt, const AVRational &in_time_base, std::string &err) {
  if (!fmt_ctx_ || !out_stream_ || !remux_mode_ || !enc_pkt_) {
    err = "HlsMuxer is not open in remux mode";
    return false;
  }
  if (!pkt) {
    return true;
  }

  if (!saw_keyframe_) {
    if ((pkt->flags & AV_PKT_FLAG_KEY) == 0) {
      return true;
    }
    saw_keyframe_ = true;
  }

  int64_t in_dts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
  int64_t in_pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : in_dts;
  if (in_dts == AV_NOPTS_VALUE) {
    in_dts = 0;
    in_pts = 0;
  }

  if (first_in_dts_ == AV_NOPTS_VALUE) {
    first_in_dts_ = in_dts;
    first_in_pts_ = in_pts;
  }

  int ret = av_packet_ref(enc_pkt_.get(), pkt);
  if (ret < 0) {
    err = "av_packet_ref failed: " + av_err_str(ret);
    return false;
  }

  const int64_t rel_dts = in_dts - first_in_dts_;
  const int64_t rel_pts = in_pts - first_in_dts_;
  int64_t       dts     = av_rescale_q(rel_dts, in_time_base, out_stream_->time_base);
  int64_t       pts     = av_rescale_q(rel_pts, in_time_base, out_stream_->time_base);

  if (last_dts_ != AV_NOPTS_VALUE && dts <= last_dts_) {
    dts = last_dts_ + 1;
  }
  if (pts < dts) {
    pts = dts;
  }
  last_dts_ = dts;

  enc_pkt_->dts = dts;
  enc_pkt_->pts = pts;
  enc_pkt_->duration =
      (pkt->duration > 0) ? av_rescale_q(pkt->duration, in_time_base, out_stream_->time_base) : 0;
  enc_pkt_->pos          = -1;
  enc_pkt_->stream_index = out_stream_->index;

  ret = av_interleaved_write_frame(fmt_ctx_.get(), enc_pkt_.get());
  av_packet_unref(enc_pkt_.get());
  if (ret < 0) {
    err = "av_interleaved_write_frame(remux) failed: " + av_err_str(ret);
    return false;
  }
  return true;
}

bool HlsMuxer::encode_and_write_frame(AVFrame *frame, std::string &err) {
  if (!fmt_ctx_ || !enc_ctx_ || !out_stream_ || remux_mode_ || !enc_pkt_) {
    err = "HlsMuxer is not open in transcode mode";
    return false;
  }

  if (frame) {
    frame->pts = next_pts_++;
  }

  const int ret = avcodec_send_frame(enc_ctx_.get(), frame);
  if (ret < 0) {
    err = "avcodec_send_frame failed: " + av_err_str(ret);
    return false;
  }

  return drain_encoder(err);
}

bool HlsMuxer::drain_encoder(std::string &err) {
  while (true) {
    int ret = avcodec_receive_packet(enc_ctx_.get(), enc_pkt_.get());
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      return true;
    }
    if (ret < 0) {
      err = "avcodec_receive_packet failed: " + av_err_str(ret);
      return false;
    }

    if (enc_pkt_->duration <= 0) {
      enc_pkt_->duration = 1;
    }
    av_packet_rescale_ts(enc_pkt_.get(), enc_ctx_->time_base, out_stream_->time_base);
    if (enc_pkt_->dts == AV_NOPTS_VALUE) {
      enc_pkt_->dts = (last_dts_ == AV_NOPTS_VALUE) ? 0 : (last_dts_ + 1);
    } else if (last_dts_ != AV_NOPTS_VALUE && enc_pkt_->dts <= last_dts_) {
      enc_pkt_->dts = last_dts_ + 1;
    }
    if (enc_pkt_->pts == AV_NOPTS_VALUE || enc_pkt_->pts < enc_pkt_->dts) {
      enc_pkt_->pts = enc_pkt_->dts;
    }
    last_dts_ = enc_pkt_->dts;

    enc_pkt_->pos          = -1;
    enc_pkt_->stream_index = out_stream_->index;

    ret = av_interleaved_write_frame(fmt_ctx_.get(), enc_pkt_.get());
    av_packet_unref(enc_pkt_.get());
    if (ret < 0) {
      err = "av_interleaved_write_frame(transcode) failed: " + av_err_str(ret);
      return false;
    }
  }
}

void HlsMuxer::close() {
  if (fmt_ctx_) {
    if (!remux_mode_ && enc_ctx_ && out_stream_ && enc_pkt_) {
      std::string ignore_err;
      if (avcodec_send_frame(enc_ctx_.get(), nullptr) >= 0) {
        drain_encoder(ignore_err);
      }
    }
    av_write_trailer(fmt_ctx_.get());
  }

  enc_pkt_.reset();
  enc_ctx_.reset();
  fmt_ctx_.reset();
  out_stream_   = nullptr;
  remux_mode_   = false;
  next_pts_     = 0;
  last_dts_     = AV_NOPTS_VALUE;
  first_in_pts_ = AV_NOPTS_VALUE;
  first_in_dts_ = AV_NOPTS_VALUE;
  saw_keyframe_ = false;
}

} //< namespace streamer
