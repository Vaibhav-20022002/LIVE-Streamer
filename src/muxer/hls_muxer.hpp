#pragma once

#include "capture/av_raii.hpp"
#include "common/config.hpp"
#include <cstdint>
#include <filesystem>
#include <string>

namespace streamer {

class HlsMuxer {
public:
  explicit HlsMuxer(const Config &cfg);
  ~HlsMuxer();

  HlsMuxer(const HlsMuxer &) = delete;
  HlsMuxer &operator=(const HlsMuxer &) = delete;

  bool open_remux(const AVStream *in_stream, int start_segment_num,
                  int init_epoch, bool append_discontinuity, std::string &err);

  bool open_transcode(int width, int height, int fps, int start_segment_num,
                      int init_epoch, bool append_discontinuity,
                      std::string &err);

  bool write_remux_packet(AVPacket *pkt, const AVRational &in_time_base,
                          std::string &err);
  bool encode_and_write_frame(AVFrame *frame, std::string &err);
  void close();

  bool is_open() const { return fmt_ctx_ != nullptr; }
  bool is_remux() const { return remux_mode_; }
  std::string init_filename() const { return init_filename_; }

private:
  bool init_hls_output(int init_epoch, std::string &err);
  bool drain_encoder(std::string &err);

  Config cfg_;
  AVFormatOutputPtr fmt_ctx_;
  AVCodecCtxPtr enc_ctx_;
  AVPacketPtr enc_pkt_;
  AVStream *out_stream_ = nullptr;
  bool remux_mode_ = false;
  std::string init_filename_ = "init_0.mp4";

  int64_t next_pts_ = 0;
  int64_t last_dts_ = AV_NOPTS_VALUE;
  int64_t first_in_pts_ = AV_NOPTS_VALUE;
  int64_t first_in_dts_ = AV_NOPTS_VALUE;
  bool saw_keyframe_ = false;
};

} //< namespace streamer
