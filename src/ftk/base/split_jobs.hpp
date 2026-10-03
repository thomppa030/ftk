#pragma once

#include "ftk/base/thread_pool.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <vector>

namespace ftk {

/// How much of a frame the owning thread's halves of split jobs may take:
/// they stop at whichever runs out first. The first step of a run always
/// goes, so a step bigger than the budget is not held back for ever.
struct FrameBudget {
    std::chrono::nanoseconds time{std::chrono::milliseconds(4)};
    uint64_t bytes{uint64_t{64} << 20};
};

/// What a worker half hands the owning thread: the step to run there, and
/// the bytes it moves (what it uploads), counted against the byte budget.
struct OwnerStep {
    std::move_only_function<void()> run;
    uint64_t bytes{0};
};

/// Work split in two: a half any thread can do (reading, decoding: data in,
/// data out) on the pool, then a half for the thread that owns what it
/// touches, run a slice each frame under a budget so a big load never
/// stalls a frame.
///
/// @code
/// jobs.submit([path] {
///     Image image = decode(path);  // on a worker
///     const uint64_t bytes = image.size();
///     return OwnerStep{[image = std::move(image)]() mutable { upload(image); }, bytes};
/// });
/// ...
/// jobs.run(budget);  // once a frame, on the owning thread
/// @endcode
class SplitJobs {
public:
    using Clock = std::chrono::steady_clock;

    explicit SplitJobs(ThreadPool& pool) : pool_(pool) {}

    /// Runs `work` on a worker; the step it returns runs on the owning thread
    /// in a later `run()`. A worker half that throws is reported and goes no
    /// further.
    void submit(std::move_only_function<OwnerStep()> work);

    /// On the owning thread: takes the steps whose worker halves are done,
    /// in the order they were submitted, then runs them until `budget` is
    /// spent. A worker half still going holds back nothing finished after it.
    /// Returns how many ran. `now` reads the time the budget is measured in.
    std::size_t run(const FrameBudget& budget, const std::function<Clock::time_point()>& now = Clock::now);

    /// Blocks until every worker half submitted so far is done; their steps
    /// wait for `run()`. For a teardown that must not leave a worker reading
    /// what is about to go.
    void wait_for_workers();

    /// Jobs submitted and not yet run on the owning thread.
    [[nodiscard]] std::size_t pending() const { return working_.size() + ready_.size(); }

private:
    /// Moves the steps of finished worker halves to `ready_`, in order.
    void take_finished();

    ThreadPool& pool_;
    // Worker halves under way, oldest first.
    std::deque<std::future<OwnerStep>> working_;
    // Steps whose worker half is done, waiting for the owning thread.
    std::deque<OwnerStep> ready_;
};

} // namespace ftk
