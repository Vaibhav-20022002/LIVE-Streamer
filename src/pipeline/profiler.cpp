#include "pipeline/profiler.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace streamer {

const char *stage_name(Stage s) {
  switch (s) {
    case Stage::Capture:
      return "Capture";
    case Stage::Decode:
      return "Decode";
    case Stage::Preprocess:
      return "Preprocess";
    case Stage::Inference:
      return "Inference";
    case Stage::Overlay:
      return "Overlay";
    case Stage::Encode:
      return "Encode";
    case Stage::Mux:
      return "Mux";
    case Stage::TotalFrame:
      return "TotalFrame";
    case Stage::Count:
      return "Unknown";
  }
  return "Unknown";
}

void StageProfiler::record(Stage stage, double ms) {
  size_t idx = static_cast<size_t>(stage);
  if (idx >= rings_.size()) {
    return;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto                       &ring = rings_[idx];

  /// Overwrite oldest sample circularly once the window is full.
  if (ring.samples.size() < kWindowSize) {
    ring.samples.push_back(ms);
  } else {
    ring.samples[ring.write_idx] = ms;
    ring.write_idx               = (ring.write_idx + 1) % kWindowSize;
  }

  ring.total_count++;
  ring.sum_ms += ms;
}

StageStats StageProfiler::snapshot(Stage stage) const {
  size_t idx = static_cast<size_t>(stage);
  if (idx >= rings_.size()) {
    return StageStats{};
  }

  std::lock_guard<std::mutex> lock(mu_);
  const auto                 &ring = rings_[idx];

  StageStats stats;
  stats.count = ring.total_count;
  if (ring.samples.empty()) {
    return stats;
  }

  stats.avg_ms = ring.sum_ms / static_cast<double>(ring.total_count);

  std::vector<double> sorted = ring.samples;
  std::sort(sorted.begin(), sorted.end());

  /// Linearly interpolate between adjacent ranks for smooth percentiles.
  auto get_percentile = [&](double p) -> double {
    if (sorted.empty()) return 0.0;
    double rank      = p * (sorted.size() - 1);
    size_t lower_idx = static_cast<size_t>(std::floor(rank));
    size_t upper_idx = static_cast<size_t>(std::ceil(rank));
    if (lower_idx >= sorted.size()) lower_idx = sorted.size() - 1;
    if (upper_idx >= sorted.size()) upper_idx = sorted.size() - 1;
    double weight = rank - lower_idx;
    return sorted[lower_idx] * (1.0 - weight) + sorted[upper_idx] * weight;
  };

  stats.p50_ms = get_percentile(0.50);
  stats.p95_ms = get_percentile(0.95);
  stats.p99_ms = get_percentile(0.99);

  return stats;
}

std::string StageProfiler::format_summary() const {
  std::ostringstream ss;
  ss << "=== Stage Profiler Summary ===\n";
  ss << std::fixed << std::setprecision(2);

  for (size_t i = 0; i < static_cast<size_t>(Stage::Count); ++i) {
    Stage      s  = static_cast<Stage>(i);
    StageStats st = snapshot(s);
    ss << std::setw(12) << stage_name(s) << " | "
       << "Count: " << std::setw(6) << st.count << " | "
       << "Avg: " << std::setw(7) << st.avg_ms << " ms | "
       << "P50: " << std::setw(7) << st.p50_ms << " ms | "
       << "P95: " << std::setw(7) << st.p95_ms << " ms | "
       << "P99: " << std::setw(7) << st.p99_ms << " ms\n";
  }
  return ss.str();
}

void StageProfiler::reset() {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto &ring : rings_) {
    ring.samples.clear();
    ring.write_idx   = 0;
    ring.total_count = 0;
    ring.sum_ms      = 0.0;
  }
}

} //< namespace streamer
