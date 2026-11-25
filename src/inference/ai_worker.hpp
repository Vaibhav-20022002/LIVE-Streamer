#pragma once

#include "common/config.hpp"
#include "inference/detection.hpp"
#include "pipeline/buffer_pool.hpp"
#include "pipeline/profiler.hpp"
#include "pipeline/ring_buffer.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace streamer {

class AiWorker {
public:
  AiWorker(const Config &cfg, StageProfiler &profiler);
  ~AiWorker();

  AiWorker(const AiWorker &)            = delete;
  AiWorker &operator=(const AiWorker &) = delete;

  void start();
  void stop();

  /// Asynchronous non-blocking submission (drops oldest pending frame on
  /// backpressure).
  void submit_rgb_frame(const uint8_t *rgb_data, int stride, int width, int height);

  /// Synchronous inference on caller thread (used for Baseline profiling
  /// comparison).
  std::vector<Detection> infer_sync(const uint8_t *rgb_data, int stride, int width, int height);

  std::vector<Detection> latest_detections() const;
  std::string            backend_name() const { return backend_name_; }
  bool                   is_active() const { return active_; }
  uint64_t               inference_count() const { return inference_count_.load(); }
  uint64_t               dropped_jobs() const { return queue_.dropped_count(); }

private:
  struct InferenceJob {
    std::shared_ptr<std::vector<uint8_t>> rgb_buf;
    int                                   width  = 0;
    int                                   height = 0;
    uint64_t                              seq    = 0;
  };

  struct Impl;
  bool
  prepare_job(const uint8_t *rgb_data, int stride, int width, int height, InferenceJob &out_job);
  bool
  prepare_job(const uint8_t *rgb_data, int stride, int width, int height, InferenceJob &out_job);
  void                   worker_loop();
  std::vector<Detection> run_inference_on_job(const InferenceJob &job);
  std::vector<Detection> generate_synthetic_detections(int width, int height, uint64_t seq) const;

  Config                             cfg_;
  StageProfiler                     &profiler_;
  DropOldestRingBuffer<InferenceJob> queue_{2};
  std::shared_ptr<ByteBufferPool>    buffer_pool_;
  std::unique_ptr<Impl>              impl_;
  std::thread                        thread_;
  std::atomic<bool>                  running_{false};
  bool                               active_       = false;
  std::string                        backend_name_ = "disabled";
  std::atomic<uint64_t>              job_seq_{0};
  std::atomic<uint64_t>              inference_count_{0};

  mutable std::mutex     det_mu_;
  std::vector<Detection> latest_detections_;
};

} // namespace streamer
