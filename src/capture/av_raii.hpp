#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace streamer {

struct AVFormatInputDeleter {
  void operator()(AVFormatContext *ctx) const noexcept {
    if (ctx) {
      avformat_close_input(&ctx);
    }
  }
};

struct AVFormatOutputDeleter {
  void operator()(AVFormatContext *ctx) const noexcept {
    if (ctx) {
      if (ctx->pb && !(ctx->oformat && (ctx->oformat->flags & AVFMT_NOFILE))) {
        avio_closep(&ctx->pb);
      }
      avformat_free_context(ctx);
    }
  }
};

struct AVCodecCtxDeleter {
  void operator()(AVCodecContext *ctx) const noexcept {
    if (ctx) {
      avcodec_free_context(&ctx);
    }
  }
};

struct AVFrameDeleter {
  void operator()(AVFrame *frame) const noexcept {
    if (frame) {
      av_frame_free(&frame);
    }
  }
};

struct AVPacketDeleter {
  void operator()(AVPacket *pkt) const noexcept {
    if (pkt) {
      av_packet_free(&pkt);
    }
  }
};

struct SwsCtxDeleter {
  void operator()(SwsContext *ctx) const noexcept {
    if (ctx) {
      sws_freeContext(ctx);
    }
  }
};

struct AVDictionaryDeleter {
  void operator()(AVDictionary *dict) const noexcept {
    if (dict) {
      av_dict_free(&dict);
    }
  }
};

using AVFormatInputPtr = std::unique_ptr<AVFormatContext, AVFormatInputDeleter>;
using AVFormatOutputPtr =
    std::unique_ptr<AVFormatContext, AVFormatOutputDeleter>;
using AVCodecCtxPtr = std::unique_ptr<AVCodecContext, AVCodecCtxDeleter>;
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;
using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
using SwsCtxPtr = std::unique_ptr<SwsContext, SwsCtxDeleter>;
using AVDictionaryPtr = std::unique_ptr<AVDictionary, AVDictionaryDeleter>;

inline std::string av_err_str(int errnum) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(errnum, buf, sizeof(buf));
  return std::string(buf);
}

inline int64_t now_steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline int64_t now_epoch_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} //< namespace streamer
