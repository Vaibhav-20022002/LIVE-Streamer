#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace streamer {

/// Bounded queue that drops the oldest element when full.
template<typename T> class DropOldestRingBuffer {
public:
  explicit DropOldestRingBuffer(size_t capacity)
      : capacity_(capacity > 0 ? capacity : 1) {}

  bool push(T item) {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopped_) {
      return false;
    }
    bool dropped = false;
    if (queue_.size() >= capacity_) {
      queue_.pop_front();
      ++dropped_count_;
      dropped = true;
    }
    queue_.push_back(std::move(item));
    cv_.notify_one();
    return dropped;
  }

  bool pop_wait(T &out) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return !queue_.empty() || stopped_; });
    if (queue_.empty()) {
      return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  bool try_pop(T &out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_.empty()) {
      return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  void shutdown() {
    std::lock_guard<std::mutex> lock(mu_);
    stopped_ = true;
    cv_.notify_all();
  }

  void reset() {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.clear();
    stopped_       = false;
    dropped_count_ = 0;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
  }

  size_t capacity() const { return capacity_; }

  uint64_t dropped_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return dropped_count_;
  }

private:
  size_t                  capacity_;
  mutable std::mutex      mu_;
  std::condition_variable cv_;
  std::deque<T>           queue_;
  bool                    stopped_       = false;
  uint64_t                dropped_count_ = 0;
};

} //< namespace streamer
