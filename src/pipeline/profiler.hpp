#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace streamer {

enum class Stage : size_t {
  Capture    = 0,
  Decode     = 1,
  Preprocess = 2,
  Inference  = 3,
  Overlay    = 4,
  Encode     = 5,
  Mux        = 6,
  TotalFrame = 7,
  Count      = 8
};

struct StageStats {
  double   avg_ms = 0.0;
  double   p50_ms = 0.0;
  double   p95_ms = 0.0;
  double   p99_ms = 0.0;
  uint64_t count  = 0;
};

/// Rolling-window latency profiler for pipeline stages.
class StageProfiler {
public:
  static constexpr size_t kWindowSize = 300; //< Max samples per stage.

  void record(Stage stage, double ms);

  StageStats snapshot(Stage stage) const;

  std::string format_summary() const;

  void reset();

  /// RAII timer that records elapsed stage duration on destruction.
  class ScopedTimer {
  public:
    ScopedTimer(StageProfiler &profiler, Stage stage)
        : profiler_(profiler)
        , stage_(stage)
        , start_(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() {
      auto   end = std::chrono::steady_clock::now();
      double ms  = std::chrono::duration<double, std::milli>(end - start_).count();
      profiler_.record(stage_, ms);
    }

    ScopedTimer(const ScopedTimer &)            = delete;
    ScopedTimer &operator=(const ScopedTimer &) = delete;

  private:
    StageProfiler                        &profiler_;
    Stage                                 stage_;
    std::chrono::steady_clock::time_point start_;
  };

private:
  struct Ring {
    std::vector<double> samples;
    size_t              write_idx   = 0;
    uint64_t            total_count = 0;
    double              sum_ms      = 0.0;
  };

  mutable std::mutex                                  mu_;
  std::array<Ring, static_cast<size_t>(Stage::Count)> rings_;
};

const char *stage_name(Stage s);
double      peak_rss_mb();

} //< namespace streamer
