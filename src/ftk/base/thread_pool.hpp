#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace ftk {

class ThreadPool {
public:
    explicit ThreadPool(uint32_t thread_count = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// Queues func behind the tasks already waiting and returns its result as
    /// a future.
    template <typename F>
    [[nodiscard]] auto submit(F&& func) -> std::future<std::invoke_result_t<F>>;

    /// Runs func(chunk_id, chunk_begin, chunk_end) over [begin, end) and
    /// returns once every item has run. The range is split into chunks of at
    /// least `grain` items, at most thread_count() + 1 of them. chunk_id is
    /// below thread_count() + 1 and each chunk runs on one thread, so
    /// per-chunk scratch may be indexed by it.
    ///
    /// The calling thread runs chunks too, including any no worker has started,
    /// so a worker busy with a long task, or one that has lost its core to
    /// another process, holds up only a chunk it had already begun. `grain` is
    /// the smallest range worth handing to another thread: a range of fewer
    /// than two grains runs here as one chunk.
    template <typename F>
    void parallel_for(uint32_t begin, uint32_t end, uint32_t grain, F&& func);

    [[nodiscard]] uint32_t thread_count() const { return thread_count_; }

private:
    /// The chunks of one parallel_for, shared by the calling thread and the
    /// workers asked to help. Workers hold a share of it, so one that starts
    /// after the call has returned finds nothing left to claim and leaves.
    struct Fork {
        uint32_t chunks{0};
        std::atomic<uint32_t> next{0};      ///< the next chunk to claim
        std::atomic<uint32_t> finished{0};  ///< chunks that have run
        /// Runs one chunk. `body` is on the calling thread's stack and is
        /// reached only through a claimed chunk, which the caller waits for.
        void (*run)(void* body, uint32_t chunk){nullptr};
        void* body{nullptr};
    };

    /// Runs chunks of `fork` until none are left to claim.
    static void work_on(Fork& fork);
    /// Blocks until every chunk of `fork` has run.
    static void wait_finished(const Fork& fork);
    /// Asks `helpers` workers to work on `fork`, ahead of the queued tasks:
    /// someone is waiting on a fork, and on nothing else in the queue.
    void post_helpers(const std::shared_ptr<Fork>& fork, uint32_t helpers);

    std::vector<std::thread> workers_;
    std::deque<std::move_only_function<void()>> tasks_;
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
        tasks_.emplace_back([t = std::move(task)]() mutable { t(); });
    }
    cv_.notify_one();
    return future;
}

template <typename F>
void ThreadPool::parallel_for(uint32_t begin, uint32_t end, uint32_t grain, F&& func) {
    if (begin >= end) return;

    const uint32_t total = end - begin;
    const uint32_t chunks = std::min(thread_count_ + 1, total / std::max(grain, 1u));
    if (chunks < 2) {
        func(0u, begin, end);
        return;
    }

    // Chunk c covers [c * total / chunks, (c + 1) * total / chunks) of the
    // range, so no two chunks differ by more than one item.
    auto body = [&func, begin, total, chunks](uint32_t chunk) {
        auto edge = [&](uint32_t c) {
            return begin + static_cast<uint32_t>(uint64_t{c} * total / chunks);
        };
        func(chunk, edge(chunk), edge(chunk + 1));
    };

    auto fork = std::make_shared<Fork>();
    fork->chunks = chunks;
    fork->body = &body;
    fork->run = [](void* b, uint32_t chunk) { (*static_cast<decltype(body)*>(b))(chunk); };

    post_helpers(fork, chunks - 1);
    work_on(*fork);
    wait_finished(*fork);
}

} // namespace ftk
