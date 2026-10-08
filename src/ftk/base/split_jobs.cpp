#include "ftk/base/split_jobs.hpp"
#include "ftk/base/log.hpp"

#include <exception>

namespace ftk {

void SplitJobs::submit(std::move_only_function<OwnerStep()> work) {
    working_.push_back(pool_.submit(std::move(work)));
}

void SplitJobs::take_finished() {
    for (auto it = working_.begin(); it != working_.end();) {
        if (it->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        try {
            ready_.push_back(it->get());
        } catch (const std::exception& e) {
            FTK_CORE_ERROR("A worker's half of a job failed: {}", e.what());
        }
        it = working_.erase(it);
    }
}

void SplitJobs::wait_for_workers() {
    for (auto& work : working_) work.wait();
    take_finished();
}

void SplitJobs::discard() {
    wait_for_workers();
    ready_.clear();
}

std::size_t SplitJobs::run(const FrameBudget& budget, const std::function<Clock::time_point()>& now) {
    take_finished();

    const Clock::time_point start = now();
    uint64_t bytes = 0;
    std::size_t ran = 0;
    while (!ready_.empty()) {
        if (ran > 0 && (now() - start >= budget.time || bytes + ready_.front().bytes > budget.bytes)) break;
        OwnerStep step = std::move(ready_.front());
        ready_.pop_front();
        bytes += step.bytes;
        if (step.run) step.run();
        ++ran;
    }
    return ran;
}

bool SplitJobs::run_until(const std::function<bool()>& done) {
    constexpr FrameBudget UNBUDGETED{.time = std::chrono::nanoseconds::max(), .bytes = ~uint64_t{0}};
    while (!done()) {
        if (pending() == 0) return false;
        // Nothing finished yet: the oldest worker half is waited for, and
        // whatever else finished meanwhile runs with it.
        if (run(UNBUDGETED) == 0 && !working_.empty()) working_.front().wait();
    }
    return true;
}

} // namespace ftk
