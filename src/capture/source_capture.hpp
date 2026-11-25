#pragma once

#include "capture/av_raii.hpp"
#include "common/config.hpp"
#include <atomic>
#include <cstdint>
#include <string>

namespace streamer {

struct InterruptState {
  std::atomic<int64_t> deadline_ms{0};
  std::atomic<bool> *stop_flag = nullptr;
};

enum class ReadStatus { Ok, Eof, Timeout, Error, Stopped };

class SourceCapture {
public:
  SourceCapture(const Config &cfg, std::atomic<bool> &stop_flag);
  ~SourceCapture();

  SourceCapture(const SourceCapture &) = delete;
  SourceCapture &operator=(const SourceCapture &) = delete;

  bool open(std::string &err);
  void close();

  ReadStatus read_packet(AVPacket *pkt, std::string &err);

  bool is_open() const { return fmt_ctx_ != nullptr; }
  bool can_remux() const;

  int video_stream_index() const { return video_stream_idx_; }
  const AVStream *video_stream() const;
  int width() const { return width_; }
  int height() const { return height_; }
  int fps() const { return fps_; }
  AVCodecID codec_id() const { return codec_id_; }

private:
  static int avio_interrupt_cb(void *opaque);
  void arm_timeout();

  Config cfg_;
  InterruptState interrupt_state_;
  AVFormatInputPtr fmt_ctx_;
  int video_stream_idx_ = -1;
  int width_ = 0;
  int height_ = 0;
  int fps_ = 30;
  AVCodecID codec_id_ = AV_CODEC_ID_NONE;
  int64_t next_pace_ms_ = 0;
};

} //< namespace streamer
