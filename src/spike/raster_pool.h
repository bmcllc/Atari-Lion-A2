#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace lion {

// Small fork/join pool: run(n, fn) calls fn(0..n-1) across worker threads and
// waits for all of them. Used to rasterize disjoint row bands in parallel.
class RasterPool {
 public:
  explicit RasterPool(unsigned threads) {
    for (unsigned i = 0; i < threads; ++i) workers_.emplace_back([this] { loop(); });
  }
  ~RasterPool() {
    {
      std::lock_guard lock(m_);
      quit_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
  }
  unsigned size() const { return unsigned(workers_.size()); }

  void run(unsigned tasks, const std::function<void(unsigned)>& fn) {
    {
      std::lock_guard lock(m_);
      fn_ = &fn;
      next_ = 0;
      tasks_ = tasks;
      pending_ = tasks;
      ++generation_;
    }
    cv_.notify_all();
    work();  // the caller helps too
    std::unique_lock lock(m_);
    done_cv_.wait(lock, [&] { return pending_ == 0; });
    fn_ = nullptr;
  }

 private:
  void work() {
    for (;;) {
      unsigned task;
      const std::function<void(unsigned)>* fn;
      {
        std::lock_guard lock(m_);
        if (!fn_ || next_ >= tasks_) return;
        task = next_++;
        fn = fn_;
      }
      (*fn)(task);
      std::lock_guard lock(m_);
      if (--pending_ == 0) done_cv_.notify_all();
    }
  }
  void loop() {
    unsigned seen = 0;
    for (;;) {
      {
        std::unique_lock lock(m_);
        cv_.wait(lock, [&] { return quit_ || (generation_ != seen && fn_ && next_ < tasks_); });
        if (quit_) return;
        seen = generation_;
      }
      work();
    }
  }

  std::vector<std::thread> workers_;
  std::mutex m_;
  std::condition_variable cv_, done_cv_;
  const std::function<void(unsigned)>* fn_ = nullptr;
  unsigned next_ = 0, tasks_ = 0, pending_ = 0, generation_ = 0;
  bool quit_ = false;
};

}  // namespace lion
