#pragma once

/**
 * @file ring_buffer.hpp
 * @brief Thread-safe bounded ring buffer implementing a drop-oldest real-time
 * backpressure policy.
 *
 * Role in Pipeline:
 * Acts as an asynchronous handoff queue between pipeline stages (e.g.,
 * capture/decode -> AI inference), ensuring slow consumers never block
 * real-time producers.
 */

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace streamer {

/// Thread-safe bounded queue that drops the oldest element on overflow
/// so real-time producers (capture/decode) never block on slow consumers (AI).
template <typename T> class DropOldestRingBuffer {
public:
  explicit DropOldestRingBuffer(size_t capacity)
      : capacity_(capacity > 0 ? capacity : 1) {}

  /// Pushes an item into the ring buffer.
  /// Backpressure Policy (Drop-Oldest):
  /// - If the queue has reached `capacity_`, the oldest element at the front is
  /// evicted (`pop_front()`),
  ///   and `dropped_count_` is incremented. This guarantees real-time producers
  ///   never block.
  /// - Notifies waiting consumer threads via condition variable
  /// (`cv_.notify_one()`).
  /// - Returns true if an older item was evicted.
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

  /// Blocks until an item is available in the queue or `shutdown()` is called.
  /// Shutdown & Wakeup Semantics:
  /// - The predicate checks `!queue_.empty() || stopped_`. When `shutdown()`
  /// sets `stopped_ = true`
  ///   and calls `cv_.notify_all()`, sleeping worker threads wake up
  ///   immediately.
  /// - If `queue_` is empty upon wakeup (due to shutdown), returns false.
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

  /// Attempts to pop an item without blocking. Returns false if the queue is
  /// empty.
  bool try_pop(T &out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_.empty()) {
      return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  /// Signals shutdown, setting `stopped_ = true` and waking up all blocked
  /// consumer threads.
  void shutdown() {
    std::lock_guard<std::mutex> lock(mu_);
    stopped_ = true;
    cv_.notify_all();
  }

  /// Clears the queue, resets the stopped flag, and resets drop counters.
  void reset() {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.clear();
    stopped_ = false;
    dropped_count_ = 0;
  }

  /// Returns current number of items in the queue.
  size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
  }

  /// Returns maximum capacity of the ring buffer.
  size_t capacity() const { return capacity_; }

  /// Returns total number of dropped/evicted items due to overflow.
  uint64_t dropped_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return dropped_count_;
  }

private:
  size_t capacity_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<T> queue_;
  bool stopped_ = false;
  uint64_t dropped_count_ = 0;
};

} //< namespace streamer
