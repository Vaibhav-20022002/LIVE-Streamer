#pragma once

/**
 * @file buffer_pool.hpp
 * @brief Thread-safe reusable byte-buffer pool for the video streaming
 * pipeline.
 *
 * Role in Pipeline:
 * Eliminates per-frame heap allocation overhead across capture, decode, and
 * encode stages by pre-allocating and recycling fixed-size vector buffers.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace streamer {

/// Thread-safe reusable byte-buffer pool to avoid per-frame heap allocations.
/// Inherits from std::enable_shared_from_this to issue custom deleters that
/// safely return buffers to the pool even if the pool outlives or is destroyed
/// alongside clients.
class ByteBufferPool : public std::enable_shared_from_this<ByteBufferPool> {
public:
  /// Factory method to create a shared instance of ByteBufferPool.
  static std::shared_ptr<ByteBufferPool>
  create(size_t buffer_bytes, size_t initial_count, size_t max_count = 16) {
    return std::shared_ptr<ByteBufferPool>(
        new ByteBufferPool(buffer_bytes, initial_count, max_count));
  }

  /// Acquires a buffer from the pool (or allocates a new one if pool is empty).
  /// Attaches a custom std::shared_ptr deleter wrapping a
  /// std::weak_ptr<ByteBufferPool>. Lifecycle & Destruction Safety:
  /// - If the pool is still alive when the buffer is destroyed, the custom
  /// deleter invokes release().
  /// - If the pool has already been destroyed, weak_ptr::lock() returns
  /// nullptr, and the deleter
  ///   safely falls back to `delete ptr` to prevent dangling pointer access or
  ///   use-after-free.
  std::shared_ptr<std::vector<uint8_t>> acquire() {
    std::vector<uint8_t> *raw = nullptr;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!free_list_.empty()) {
        raw = free_list_.back().release();
        free_list_.pop_back();
      }
    }
    if (!raw) {
      raw = new std::vector<uint8_t>(buffer_bytes_);
    } else if (raw->size() != buffer_bytes_) {
      raw->resize(buffer_bytes_);
    }

    std::weak_ptr<ByteBufferPool> weak_self = weak_from_this();
    return std::shared_ptr<std::vector<uint8_t>>(
        raw, [weak_self](std::vector<uint8_t> *ptr) {
          if (auto self = weak_self.lock()) {
            self->release(ptr);
          } else {
            delete ptr;
          }
        });
  }

  /// Returns the number of currently available buffers in the free list.
  size_t available_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return free_list_.size();
  }

  /// Returns the configured capacity (in bytes) of each managed buffer.
  size_t buffer_bytes() const { return buffer_bytes_; }

private:
  ByteBufferPool(size_t buffer_bytes, size_t initial_count, size_t max_count)
      : buffer_bytes_(buffer_bytes), max_count_(max_count) {
    free_list_.reserve(initial_count);
    for (size_t i = 0; i < initial_count; ++i) {
      free_list_.push_back(
          std::make_unique<std::vector<uint8_t>>(buffer_bytes_));
    }
  }

  /// Returns a buffer back to the free list if below max_count_; otherwise
  /// deallocates it.
  void release(std::vector<uint8_t> *ptr) {
    std::unique_ptr<std::vector<uint8_t>> owned(ptr);
    std::lock_guard<std::mutex> lock(mu_);
    if (free_list_.size() < max_count_) {
      free_list_.push_back(std::move(owned));
    }
  }

  size_t buffer_bytes_;
  size_t max_count_;
  mutable std::mutex mu_;
  std::vector<std::unique_ptr<std::vector<uint8_t>>> free_list_;
};

} //< namespace streamer
