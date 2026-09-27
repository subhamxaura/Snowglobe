#pragma once
// Bounded blocking byte queue between producer and consumer threads.
// push() blocks when full (backpressure); pop() blocks until an item or
// close(); close() wakes everyone. TSan-clean by construction (one mutex).
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

namespace snowglobe::proxy {

class BlockingQueue {
public:
  // Contract: no single item exceeds capacityBytes (callers chunk to 32 KiB
  // against a 1 MiB cap); an oversized item would wait until close().
  explicit BlockingQueue(size_t capacityBytes) : cap_(capacityBytes) {}

  BlockingQueue(const BlockingQueue&) = delete;
  BlockingQueue& operator=(const BlockingQueue&) = delete;

  void push(std::string item) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&] { return used_ + item.size() <= cap_ || closed_; });
    if (closed_) {
      return;
    }
    used_ += item.size();
    q_.push_back(std::move(item));
    cv_.notify_one();
  }

  // Returns false iff empty AND closed (consumer drains, then stops).
  bool pop(std::string& out) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&] { return !q_.empty() || closed_; });
    if (q_.empty()) {
      return false;
    }
    out = std::move(q_.front());
    q_.pop_front();
    used_ -= out.size();
    cv_.notify_one();
    return true;
  }

  void close() {
    std::lock_guard<std::mutex> lk(mu_);
    closed_ = true;
    cv_.notify_all();
  }

private:
  const size_t cap_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::string> q_;
  size_t used_ = 0;
  bool closed_ = false;
};

} // namespace snowglobe::proxy
