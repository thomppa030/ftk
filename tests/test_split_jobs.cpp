#include "ftk/base/split_jobs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

using ftk::FrameBudget;
using ftk::OwnerStep;
using ftk::SplitJobs;
using namespace std::chrono_literals;

namespace {

// A budget that never runs out, for tests about something else.
constexpr FrameBudget UNLIMITED{.time = std::chrono::hours(1), .bytes = ~uint64_t{0}};

} // namespace

TEST_CASE("a job's worker half runs on a worker and its step on the thread that runs them", "[base][split_jobs]") {
    ftk::ThreadPool pool(2);
    SplitJobs jobs(pool);
    std::thread::id worker;
    std::thread::id owner;
    jobs.submit([&] {
        worker = std::this_thread::get_id();
        return OwnerStep{[&] { owner = std::this_thread::get_id(); }, 0};
    });
    jobs.wait_for_workers();
    CHECK(jobs.pending() == 1);
    CHECK(jobs.run(UNLIMITED) == 1);
    CHECK(worker != std::this_thread::get_id());
    CHECK(owner == std::this_thread::get_id());
    CHECK(jobs.pending() == 0);
}

TEST_CASE("steps run in the order their jobs were submitted", "[base][split_jobs]") {
    ftk::ThreadPool pool(4);
    SplitJobs jobs(pool);
    std::vector<int> order;
    for (int i = 0; i < 6; ++i) {
        jobs.submit([&order, i] { return OwnerStep{[&order, i] { order.push_back(i); }, 0}; });
    }
    jobs.wait_for_workers();
    CHECK(jobs.run(UNLIMITED) == 6);
    CHECK(order == std::vector<int>{0, 1, 2, 3, 4, 5});
}

TEST_CASE("a frame's steps stop once their bytes would pass the budget", "[base][split_jobs]") {
    ftk::ThreadPool pool(1);
    SplitJobs jobs(pool);
    int ran = 0;
    for (int i = 0; i < 3; ++i) jobs.submit([&ran] { return OwnerStep{[&ran] { ++ran; }, 40}; });
    jobs.wait_for_workers();
    const FrameBudget budget{.time = std::chrono::hours(1), .bytes = 64};
    CHECK(jobs.run(budget) == 1);
    CHECK(jobs.run(budget) == 1);
    CHECK(jobs.run(budget) == 1);
    CHECK(ran == 3);
}

TEST_CASE("the first step of a frame goes even when it is bigger than the budget", "[base][split_jobs]") {
    ftk::ThreadPool pool(1);
    SplitJobs jobs(pool);
    jobs.submit([] { return OwnerStep{[] {}, 1000}; });
    jobs.submit([] { return OwnerStep{[] {}, 1}; });
    jobs.wait_for_workers();
    const FrameBudget budget{.time = std::chrono::hours(1), .bytes = 64};
    CHECK(jobs.run(budget) == 1);
    CHECK(jobs.run(budget) == 1);
}

TEST_CASE("a frame's steps stop once their time is spent", "[base][split_jobs]") {
    ftk::ThreadPool pool(1);
    SplitJobs jobs(pool);
    for (int i = 0; i < 5; ++i) jobs.submit([] { return OwnerStep{[] {}, 0}; });
    jobs.wait_for_workers();
    // Each reading of the clock is 3 ms after the last: a 4 ms budget lets
    // a step start at 0 ms and at 3 ms, and stops the third at 6 ms.
    SplitJobs::Clock::time_point clock{};
    const auto now = [&clock] { return clock += 3ms; };
    const FrameBudget budget{.time = 4ms, .bytes = ~uint64_t{0}};
    CHECK(jobs.run(budget, now) == 2);
    CHECK(jobs.pending() == 3);
}

TEST_CASE("a worker half that throws is reported and the rest go on", "[base][split_jobs]") {
    ftk::ThreadPool pool(2);
    SplitJobs jobs(pool);
    int ran = 0;
    jobs.submit([]() -> OwnerStep { throw std::runtime_error("the file is gone"); });
    jobs.submit([&ran] { return OwnerStep{[&ran] { ++ran; }, 0}; });
    jobs.wait_for_workers();
    CHECK(jobs.run(UNLIMITED) == 1);
    CHECK(ran == 1);
    CHECK(jobs.pending() == 0);
}
