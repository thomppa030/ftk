#include "core/thread_pool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <mutex>
#include <thread>
#include <vector>

using namespace fjell;

namespace {

struct ChunkRun {
    uint32_t id;
    uint32_t begin;
    uint32_t end;
    std::thread::id thread;
};

/// Every chunk parallel_for ran over [begin, end), in range order.
std::vector<ChunkRun> run_chunks(ThreadPool& pool, uint32_t begin, uint32_t end, uint32_t grain) {
    std::mutex mutex;
    std::vector<ChunkRun> runs;
    pool.parallel_for(begin, end, grain, [&](uint32_t id, uint32_t b, uint32_t e) {
        std::lock_guard lock(mutex);
        runs.push_back({id, b, e, std::this_thread::get_id()});
    });
    std::ranges::sort(runs, {}, &ChunkRun::begin);
    return runs;
}

/// Holds every worker of a pool in a task until release(): the pool as it is
/// when its workers are busy with long tasks or have lost their cores.
class BusyWorkers {
public:
    explicit BusyWorkers(ThreadPool& pool) : started_(pool.thread_count()) {
        std::shared_future<void> gate = gate_.get_future().share();
        for (uint32_t i = 0; i < pool.thread_count(); ++i) {
            held_.push_back(pool.submit([this, gate] {
                started_.count_down();
                gate.wait();
            }));
        }
        started_.wait();
    }

    void release() {
        gate_.set_value();
        for (auto& task : held_) task.wait();
    }

private:
    std::latch started_;
    std::promise<void> gate_;
    std::vector<std::future<void>> held_;
};

} // namespace

TEST_CASE("parallel_for runs every item once, in chunks of at least the grain", "[thread_pool]") {
    ThreadPool pool(3);
    const uint32_t most_chunks = pool.thread_count() + 1;
    constexpr uint32_t first = 10;

    for (uint32_t total : {1u, 2u, 3u, 7u, 8u, 9u, 63u, 64u, 127u, 128u, 1000u}) {
        for (uint32_t grain : {1u, 3u, 64u}) {
            CAPTURE(total, grain);
            const auto runs = run_chunks(pool, first, first + total, grain);

            REQUIRE_FALSE(runs.empty());
            REQUIRE(runs.size() <= most_chunks);
            REQUIRE(runs.front().begin == first);
            REQUIRE(runs.back().end == first + total);
            std::vector<bool> id_seen(most_chunks, false);
            for (std::size_t k = 0; k < runs.size(); ++k) {
                if (k > 0) REQUIRE(runs[k].begin == runs[k - 1].end);
                REQUIRE(runs[k].id < most_chunks);
                REQUIRE_FALSE(id_seen[runs[k].id]);
                id_seen[runs[k].id] = true;
                if (runs.size() > 1) REQUIRE(runs[k].end - runs[k].begin >= grain);
            }
            if (total < 2 * grain) {
                REQUIRE(runs.size() == 1);
                REQUIRE(runs.front().id == 0);
                REQUIRE(runs.front().thread == std::this_thread::get_id());
            }
        }
    }
}

TEST_CASE("parallel_for over an empty range runs nothing", "[thread_pool]") {
    ThreadPool pool(2);
    REQUIRE(run_chunks(pool, 5, 5, 1).empty());
}

TEST_CASE("parallel_for does not wait for workers that never start", "[thread_pool]") {
    // Counted outside the call: a helper that starts after parallel_for has
    // returned must find nothing left to run.
    static std::atomic<uint32_t> chunk_calls{0};
    chunk_calls = 0;
    std::vector<ChunkRun> runs;
    {
        ThreadPool pool(3);
        BusyWorkers busy(pool);

        std::mutex mutex;
        pool.parallel_for(0, 1000, 1, [&](uint32_t id, uint32_t b, uint32_t e) {
            chunk_calls.fetch_add(1);
            std::lock_guard lock(mutex);
            runs.push_back({id, b, e, std::this_thread::get_id()});
        });

        REQUIRE(runs.size() == pool.thread_count() + 1);
        for (const auto& run : runs) REQUIRE(run.thread == std::this_thread::get_id());

        busy.release();
    } // joins the workers, after they have run the helpers queued for the call
    REQUIRE(chunk_calls.load() == runs.size());
}

TEST_CASE("parallel_for inside worker tasks finishes when every worker calls it", "[thread_pool]") {
    ThreadPool pool(2);
    std::atomic<uint32_t> items{0};
    std::vector<std::future<void>> tasks;
    for (uint32_t t = 0; t < pool.thread_count(); ++t) {
        tasks.push_back(pool.submit([&] {
            pool.parallel_for(0, 100, 1, [&](uint32_t, uint32_t b, uint32_t e) {
                items.fetch_add(e - b);
            });
        }));
    }
    for (auto& task : tasks) {
        REQUIRE(task.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    }
    REQUIRE(items.load() == 100 * pool.thread_count());
}
