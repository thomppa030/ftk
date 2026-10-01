#include "ftk/base/thread_pool.hpp"
#include "ftk/base/log.hpp"

namespace fjell {

ThreadPool::ThreadPool(uint32_t thread_count) {
    if (thread_count == 0) {
        auto hw = std::thread::hardware_concurrency();
        thread_count_ = hw > 1 ? hw - 1 : 1;
    } else {
        thread_count_ = thread_count;
    }

    workers_.reserve(thread_count_);
    for (uint32_t i = 0; i < thread_count_; ++i) {
        workers_.emplace_back([this] {
            while (true) {
                std::move_only_function<void()> task;
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                    if (stop_ && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop_front();
                }
                task();
            }
        });
    }

    FJELL_CORE_INFO("Thread pool: {} workers", thread_count_);
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        worker.join();
    }
}

void ThreadPool::work_on(Fork& fork) {
    for (uint32_t chunk = fork.next.fetch_add(1, std::memory_order_relaxed); chunk < fork.chunks;
         chunk = fork.next.fetch_add(1, std::memory_order_relaxed)) {
        fork.run(fork.body, chunk);
        // Only the last chunk wakes the caller; it re-reads the count after
        // any wake, so the ones before it need not.
        if (fork.finished.fetch_add(1, std::memory_order_acq_rel) + 1 == fork.chunks) {
            fork.finished.notify_one();
        }
    }
}

void ThreadPool::wait_finished(const Fork& fork) {
    for (uint32_t seen = fork.finished.load(std::memory_order_acquire); seen < fork.chunks;
         seen = fork.finished.load(std::memory_order_acquire)) {
        fork.finished.wait(seen, std::memory_order_acquire);
    }
}

void ThreadPool::post_helpers(const std::shared_ptr<Fork>& fork, uint32_t helpers) {
    {
        std::lock_guard lock(mutex_);
        for (uint32_t i = 0; i < helpers; ++i) {
            tasks_.emplace_front([fork] { work_on(*fork); });
        }
    }
    for (uint32_t i = 0; i < helpers; ++i) {
        cv_.notify_one();
    }
}

} // namespace fjell
