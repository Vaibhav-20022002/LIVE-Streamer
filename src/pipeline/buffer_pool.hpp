#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace streamer {

/// Thread-safe reusable byte-buffer pool.
class ByteBufferPool : public std::enable_shared_from_this<ByteBufferPool> {
public:
  static std::shared_ptr<ByteBufferPool>
  create(size_t buffer_bytes, size_t initial_count, size_t max_count = 16) {
    return std::shared_ptr<ByteBufferPool>(
        new ByteBufferPool(buffer_bytes, initial_count, max_count));
  }

  /// Uses weak_ptr deleter; falls back to delete if pool is destroyed.
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
    return std::shared_ptr<std::vector<uint8_t>>(raw, [weak_self](std::vector<uint8_t> *ptr) {
      if (auto self = weak_self.lock()) {
        self->release(ptr);
      } else {
        delete ptr;
      }
    });
  }

  size_t available_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return free_list_.size();
  }

  size_t buffer_bytes() const { return buffer_bytes_; }

private:
  ByteBufferPool(size_t buffer_bytes, size_t initial_count, size_t max_count)
      : buffer_bytes_(buffer_bytes)
      , max_count_(max_count) {
    free_list_.reserve(initial_count);
    for (size_t i = 0; i < initial_count; ++i) {
      free_list_.push_back(std::make_unique<std::vector<uint8_t>>(buffer_bytes_));
    }
  }

  void release(std::vector<uint8_t> *ptr) {
    std::unique_ptr<std::vector<uint8_t>> owned(ptr);
    std::lock_guard<std::mutex>           lock(mu_);
    if (free_list_.size() < max_count_) {
      free_list_.push_back(std::move(owned));
    }
  }

  size_t                                             buffer_bytes_;
  size_t                                             max_count_;
  mutable std::mutex                                 mu_;
  std::vector<std::unique_ptr<std::vector<uint8_t>>> free_list_;
};

} //< namespace streamer
