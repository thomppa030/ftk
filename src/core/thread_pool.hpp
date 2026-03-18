#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <latch>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

namespace fjell {

class ThreadPool {
public:
    explicit ThreadPool(uint32_t thread_count = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template <typename F>
    [[nodiscard]] auto submit(F&& func) -> std::future<std::invoke_result_t<F>>;

    // Splits [begin, end) across workers + calling thread, blocks until done.
    // Callback: func(uint32_t chunk_id, uint32_t begin, uint32_t end)
    template <typename F>
    void parallel_for(uint32_t begin, uint32_t end, F&& func);

    [[nodiscard]] uint32_t thread_count() const { return thread_count_; }

private:
    std::vector<std::thread> workers_;
    std::queue<std::move_only_function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_{false};
    uint32_t thread_count_{0};
};

// ── Template implementations ────────────────────────────────────────────

template <typename F>
auto ThreadPool::submit(F&& func) -> std::future<std::invoke_result_t<F>> {
    using R = std::invoke_result_t<F>;
    auto task = std::packaged_task<R()>(std::forward<F>(func));
    auto future = task.get_future();
    {
        std::lock_guard lock(mutex_);
        tasks_.emplace([t = std::move(task)]() mutable { t(); });
    }
    cv_.notify_one();
    return future;
}

template <typename F>
void ThreadPool::parallel_for(uint32_t begin, uint32_t end, F&& func) {
    if (begin >= end) return;

    uint32_t total = end - begin;
    uint32_t num_chunks = thread_count_ + 1; // workers + calling thread
    uint32_t chunk_size = (total + num_chunks - 1) / num_chunks;

    // Small range or no workers — run inline
    if (total <= chunk_size || thread_count_ == 0) {
        func(0u, begin, end);
        return;
    }

    // Count how many worker chunks we actually need
    uint32_t worker_chunks = 0;
    {
        uint32_t offset = begin;
        for (uint32_t i = 0; i < thread_count_ && offset < end; ++i) {
            uint32_t chunk_end = std::min(offset + chunk_size, end);
            if (chunk_end > offset) worker_chunks++;
            offset = chunk_end;
        }
    }

    if (worker_chunks == 0) {
        func(0u, begin, end);
        return;
    }

    std::latch done(worker_chunks);

    uint32_t offset = begin;
    uint32_t chunk_id = 0;
    for (uint32_t i = 0; i < thread_count_ && offset < end; ++i) {
        uint32_t chunk_begin = offset;
        uint32_t chunk_end = std::min(offset + chunk_size, end);
        offset = chunk_end;

        {
            std::lock_guard lock(mutex_);
            tasks_.emplace([&func, chunk_id, chunk_begin, chunk_end, &done]() {
                func(chunk_id, chunk_begin, chunk_end);
                done.count_down();
            });
        }
        cv_.notify_one();
        chunk_id++;
    }

    // Calling thread takes the remainder
    if (offset < end) {
        func(chunk_id, offset, end);
    }

    done.wait();
}

} // namespace fjell
