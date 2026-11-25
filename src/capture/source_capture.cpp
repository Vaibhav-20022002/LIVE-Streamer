#include "capture/source_capture.hpp"

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

namespace streamer {

namespace {

void ensure_avdevice_registered() {
  static std::once_flag once;
  std::call_once(once, []() { avdevice_register_all(); });
}

} //< namespace

SourceCapture::SourceCapture(const Config &cfg, std::atomic<bool> &stop_flag)
    : cfg_(cfg) {
  ensure_avdevice_registered();
  interrupt_state_.stop_flag = &stop_flag;
  interrupt_state_.deadline_ms.store(0, std::memory_order_relaxed);
  fps_ = (cfg_.source_fps > 0) ? cfg_.source_fps : 30;
}

SourceCapture::~SourceCapture() { close(); }

int SourceCapture::avio_interrupt_cb(void *opaque) {
  auto *state = static_cast<InterruptState *>(opaque);
  if (!state) {
    return 0;
  }
  if (state->stop_flag && state->stop_flag->load(std::memory_order_relaxed)) {
    return 1;
  }
  const int64_t deadline = state->deadline_ms.load(std::memory_order_relaxed);
  if (deadline > 0 && now_steady_ms() >= deadline) {
    return 1;
  }
  return 0;
}

void SourceCapture::arm_timeout() {
  if (cfg_.read_timeout_ms > 0) {
    interrupt_state_.deadline_ms.store(
        now_steady_ms() + static_cast<int64_t>(cfg_.read_timeout_ms),
        std::memory_order_relaxed);
  } else {
    interrupt_state_.deadline_ms.store(0, std::memory_order_relaxed);
  }
}

void SourceCapture::close() {
  interrupt_state_.deadline_ms.store(0, std::memory_order_relaxed);
  fmt_ctx_.reset();
  video_stream_idx_ = -1;
  width_ = 0;
  height_ = 0;
  fps_ = (cfg_.source_fps > 0) ? cfg_.source_fps : 30;
  codec_id_ = AV_CODEC_ID_NONE;
  next_pace_ms_ = 0;
}

const AVStream *SourceCapture::video_stream() const {
  if (!fmt_ctx_ || video_stream_idx_ < 0 ||
      static_cast<unsigned int>(video_stream_idx_) >= fmt_ctx_->nb_streams) {
    return nullptr;
  }
  return fmt_ctx_->streams[video_stream_idx_];
}

bool SourceCapture::open(std::string &err) {
  close();
  err.clear();

  if (interrupt_state_.stop_flag &&
      interrupt_state_.stop_flag->load(std::memory_order_relaxed)) {
    err = "Capture stopped";
    return false;
  }

  AVFormatContext *raw_ctx = avformat_alloc_context();
  if (!raw_ctx) {
    err = "Failed to allocate AVFormatContext";
    return false;
  }

  raw_ctx->interrupt_callback.callback = &SourceCapture::avio_interrupt_cb;
  raw_ctx->interrupt_callback.opaque = &interrupt_state_;

  arm_timeout();

  const AVInputFormat *in_fmt = nullptr;
  AVDictionary *raw_opts = nullptr;
  /// Ensure raw_opts is freed on any exit path.
  auto free_opts = [](AVDictionary **d) {
    if (d && *d) {
      av_dict_free(d);
    }
  };

  switch (cfg_.source_type) {
  case SourceType::Rtsp: {
    av_dict_set(&raw_opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&raw_opts, "fflags", "nobuffer", 0);
    av_dict_set(&raw_opts, "flags", "low_delay", 0);
    const int64_t timeout_us =
        static_cast<int64_t>(std::max(1, cfg_.read_timeout_ms)) * 1000LL;
    av_dict_set(&raw_opts, "timeout", std::to_string(timeout_us).c_str(), 0);
    break;
  }
  case SourceType::V4l2: {
    in_fmt = av_find_input_format("v4l2");
    if (!in_fmt) {
      free_opts(&raw_opts);
      avformat_free_context(raw_ctx);
      err = "v4l2 input format not available";
      return false;
    }
    const std::string video_size = std::to_string(cfg_.source_width) + "x" +
                                   std::to_string(cfg_.source_height);
    const std::string framerate = std::to_string(std::max(1, cfg_.source_fps));
    av_dict_set(&raw_opts, "video_size", video_size.c_str(), 0);
    av_dict_set(&raw_opts, "framerate", framerate.c_str(), 0);
    break;
  }
  case SourceType::AvFoundation: {
    in_fmt = av_find_input_format("avfoundation");
    if (!in_fmt) {
      free_opts(&raw_opts);
      avformat_free_context(raw_ctx);
      err = "avfoundation input format not available";
      return false;
    }
    const std::string video_size = std::to_string(cfg_.source_width) + "x" +
                                   std::to_string(cfg_.source_height);
    const std::string framerate = std::to_string(std::max(1, cfg_.source_fps));
    av_dict_set(&raw_opts, "video_size", video_size.c_str(), 0);
    av_dict_set(&raw_opts, "framerate", framerate.c_str(), 0);
    break;
  }
  case SourceType::Lavfi: {
    in_fmt = av_find_input_format("lavfi");
    if (!in_fmt) {
      free_opts(&raw_opts);
      avformat_free_context(raw_ctx);
      err = "lavfi input format not available";
      return false;
    }
    break;
  }
  case SourceType::File: {
    in_fmt = nullptr;
    break;
  }
  }

  int ret =
      avformat_open_input(&raw_ctx, cfg_.source_url.c_str(), in_fmt, &raw_opts);
  AVDictionaryPtr opts_guard(raw_opts);
  raw_opts = nullptr;

  if (ret < 0) {
    /// avformat_open_input frees raw_ctx on failure.
    err = "avformat_open_input failed: " + av_err_str(ret);
    interrupt_state_.deadline_ms.store(0, std::memory_order_relaxed);
    return false;
  }

  fmt_ctx_.reset(raw_ctx);

  arm_timeout();
  ret = avformat_find_stream_info(fmt_ctx_.get(), nullptr);
  if (ret < 0) {
    err = "avformat_find_stream_info failed: " + av_err_str(ret);
    close();
    return false;
  }

  const int best_idx = av_find_best_stream(fmt_ctx_.get(), AVMEDIA_TYPE_VIDEO,
                                           -1, -1, nullptr, 0);
  if (best_idx < 0) {
    err = "No video stream found: " + av_err_str(best_idx);
    close();
    return false;
  }

  video_stream_idx_ = best_idx;
  AVStream *st = fmt_ctx_->streams[video_stream_idx_];
  if (!st || !st->codecpar) {
    err = "Selected video stream has missing codec parameters";
    close();
    return false;
  }

  width_ = st->codecpar->width;
  height_ = st->codecpar->height;
  codec_id_ = st->codecpar->codec_id;

  const AVRational guessed_fr =
      av_guess_frame_rate(fmt_ctx_.get(), st, nullptr);
  if (guessed_fr.num > 0 && guessed_fr.den > 0) {
    const double fps_val = av_q2d(guessed_fr);
    fps_ = (fps_val >= 0.5) ? static_cast<int>(std::lround(fps_val))
                            : (cfg_.source_fps > 0 ? cfg_.source_fps : 30);
  } else {
    fps_ = (cfg_.source_fps > 0) ? cfg_.source_fps : 30;
  }
  fps_ = std::max(1, fps_);

  next_pace_ms_ = 0;
  interrupt_state_.deadline_ms.store(0, std::memory_order_relaxed);
  return true;
}

ReadStatus SourceCapture::read_packet(AVPacket *pkt, std::string &err) {
  err.clear();

  if (interrupt_state_.stop_flag &&
      interrupt_state_.stop_flag->load(std::memory_order_relaxed)) {
    return ReadStatus::Stopped;
  }
  if (!fmt_ctx_ || video_stream_idx_ < 0 || !pkt) {
    err = "SourceCapture is not open";
    return ReadStatus::Error;
  }

  /// Pace local File and un-throttled Lavfi sources to real-time fps without
  /// throttling live network/device streams.
  const bool is_network_url = cfg_.source_url.find("://") != std::string::npos;
  const bool should_pace =
      (cfg_.source_type == SourceType::Lavfi) ||
      (cfg_.source_type == SourceType::File && !is_network_url);
  if (should_pace && next_pace_ms_ > 0) {
    while (true) {
      if (interrupt_state_.stop_flag &&
          interrupt_state_.stop_flag->load(std::memory_order_relaxed)) {
        return ReadStatus::Stopped;
      }
      const int64_t now = now_steady_ms();
      if (now >= next_pace_ms_) {
        break;
      }
      const int64_t sleep_ms = std::min<int64_t>(50, next_pace_ms_ - now);
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
  }

  while (true) {
    if (interrupt_state_.stop_flag &&
        interrupt_state_.stop_flag->load(std::memory_order_relaxed)) {
      return ReadStatus::Stopped;
    }

    arm_timeout();
    const int ret = av_read_frame(fmt_ctx_.get(), pkt);
    const int64_t deadline =
        interrupt_state_.deadline_ms.load(std::memory_order_relaxed);
    interrupt_state_.deadline_ms.store(0, std::memory_order_relaxed);

    if (ret == AVERROR_EOF) {
      return ReadStatus::Eof;
    }
    if (ret == AVERROR_EXIT) {
      if (interrupt_state_.stop_flag &&
          interrupt_state_.stop_flag->load(std::memory_order_relaxed)) {
        return ReadStatus::Stopped;
      }
      err = "Read timed out";
      return ReadStatus::Timeout;
    }
    if (ret == AVERROR(ETIMEDOUT)) {
      err = "Read timed out: " + av_err_str(ret);
      return ReadStatus::Timeout;
    }
    if (ret < 0) {
      if (interrupt_state_.stop_flag &&
          interrupt_state_.stop_flag->load(std::memory_order_relaxed)) {
        return ReadStatus::Stopped;
      }
      if (deadline > 0 && now_steady_ms() >= deadline) {
        err = "Read timed out: " + av_err_str(ret);
        return ReadStatus::Timeout;
      }
      err = "av_read_frame failed: " + av_err_str(ret);
      return ReadStatus::Error;
    }

    if (pkt->stream_index != video_stream_idx_) {
      av_packet_unref(pkt);
      continue;
    }

    if (should_pace) {
      const int64_t step_ms = std::max<int64_t>(1, 1000 / std::max(1, fps_));
      const int64_t now = now_steady_ms();
      if (next_pace_ms_ == 0 || now > next_pace_ms_ + step_ms * 4) {
        next_pace_ms_ = now + step_ms;
      } else {
        next_pace_ms_ += step_ms;
      }
    }

    return ReadStatus::Ok;
  }
}

bool SourceCapture::can_remux() const {
  return (cfg_.source_type == SourceType::Rtsp ||
          cfg_.source_type == SourceType::File) &&
         (codec_id_ == AV_CODEC_ID_H264 || codec_id_ == AV_CODEC_ID_HEVC);
}

} //< namespace streamer
