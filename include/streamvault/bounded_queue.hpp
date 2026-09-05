#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace streamvault {

template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) throw std::invalid_argument("queue capacity must be positive");
  }

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  bool try_push(T value) {
    std::lock_guard lock(mutex_);
    if (closed_ || queue_.size() == capacity_) return false;
    queue_.push_back(std::move(value));
    if (queue_.size() > high_water_mark_) high_water_mark_ = queue_.size();
    available_.notify_one();
    return true;
  }

  // Returns false only when the closed queue has been fully drained.
  bool wait_pop(T& value) {
    std::unique_lock lock(mutex_);
    available_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) return false;
    value = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  void close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    available_.notify_all();
  }

  std::size_t high_water_mark() const {
    std::lock_guard lock(mutex_);
    return high_water_mark_;
  }

  std::size_t size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
  }

  bool closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
  }

 private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable available_;
  std::deque<T> queue_;
  std::size_t high_water_mark_{};
  bool closed_{};
};

}  // namespace streamvault
