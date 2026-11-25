#pragma once

/**
 * @file profiler.hpp
 * @brief Stage execution time profiler and performance metrics collector.
 *
 * Role in Pipeline:
 * Tracks latency per pipeline stage (capture, decode, inference, encode, mux)
 * using rolling window buffers, computing average and percentile latencies
 * (P50, P95, P99). Includes RAII ScopedTimer for automatic scope-based timing.
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace streamer {

/// Pipeline stages tracked by StageProfiler.
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

/// Aggregated statistical metrics for a single pipeline stage.
struct StageStats {
  double   avg_ms = 0.0;
  double   p50_ms = 0.0;
  double   p95_ms = 0.0;
  double   p99_ms = 0.0;
  uint64_t count  = 0;
};

/// Thread-safe profiler maintaining rolling windows of stage durations.
class StageProfiler {
public:
  static constexpr size_t kWindowSize = 300; //< Maximum samples stored per stage ring buffer.

  /// Records a duration sample (in milliseconds) for a given pipeline stage.
  void record(Stage stage, double ms);

  /// Computes and returns statistical summary (average and percentiles) for a
  /// stage.
  StageStats snapshot(Stage stage) const;

  /// Formats all stage metrics into a human-readable summary string.
  std::string format_summary() const;

  /// Resets all accumulated profiling data and ring buffers.
  void reset();

  /// RAII utility class for automatic stage timing measurement.
  class ScopedTimer {
  public:
    /// Starts the steady_clock timer upon entering scope.
    ScopedTimer(StageProfiler &profiler, Stage stage)
        : profiler_(profiler)
        , stage_(stage)
        , start_(std::chrono::steady_clock::now()) {}

    /// Stops the timer upon exiting scope, computes elapsed milliseconds,
    /// and automatically records it into the StageProfiler.
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

/// Converts a Stage enum to its string representation.
const char *stage_name(Stage s);

} //< namespace streamer
