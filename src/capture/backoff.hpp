#pragma once

#include <algorithm>
#include <cstdint>
#include <random>

namespace streamer {

/// Computes bounded exponential backoff delays with symmetric jitter.
class ExponentialBackoff {
public:
  ExponentialBackoff(int base_ms, int max_ms, double jitter_ratio = 0.25,
                     uint64_t seed = std::random_device{}())
      : base_ms_(std::max(1, base_ms)), max_ms_(std::max(base_ms_, max_ms)),
        jitter_ratio_(std::clamp(jitter_ratio, 0.0, 0.9)), rng_(seed) {}

  int next_delay_ms() {
    uint64_t factor = 1ULL << std::min<int>(attempt_, 20);
    double raw = static_cast<double>(base_ms_) * static_cast<double>(factor);
    int capped =
        static_cast<int>(std::min<double>(raw, static_cast<double>(max_ms_)));
    ++attempt_;
    if (jitter_ratio_ <= 0.0) {
      return capped;
    }
    std::uniform_real_distribution<double> dist(1.0 - jitter_ratio_,
                                                1.0 + jitter_ratio_);
    int jittered = static_cast<int>(static_cast<double>(capped) * dist(rng_));
    return std::clamp(jittered, base_ms_, max_ms_);
  }

  void reset() { attempt_ = 0; }
  int attempt_count() const { return attempt_; }

private:
  int base_ms_;
  int max_ms_;
  double jitter_ratio_;
  int attempt_ = 0;
  std::mt19937_64 rng_;
};

} //< namespace streamer
