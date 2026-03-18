#include "core/thread_pool.hpp"
#include "core/log.hpp"

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
                    tasks_.pop();
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

} // namespace fjell
