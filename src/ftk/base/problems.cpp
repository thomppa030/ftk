#include "ftk/base/problems.hpp"

#include <algorithm>
#include <filesystem>
#include <format>

namespace ftk {

namespace {

std::string with_line(std::string where, const Problem& problem) {
    if (problem.line > 0) where += std::format(":{}", problem.line);
    if (problem.line > 0 && problem.column > 0) where += std::format(":{}", problem.column);
    return where;
}

} // namespace

std::string short_where(const Problem& problem) {
    if (problem.file.empty()) return {};
    return with_line(std::filesystem::path(problem.file).filename().string(), problem);
}

std::string full_where(const Problem& problem) {
    if (problem.file.empty()) return {};
    return with_line(problem.file, problem);
}

void ProblemList::report(std::string_view source, std::vector<Problem> problems) {
    std::lock_guard lock(mutex_);
    auto found = sources_.find(source);
    if (problems.empty()) {
        if (found == sources_.end()) return;
        sources_.erase(found);
        ++revision_;
        return;
    }

    static const std::vector<ListedProblem> NONE;
    const std::vector<ListedProblem>& before = found != sources_.end() ? found->second : NONE;
    const bool unchanged = std::ranges::equal(before, problems, [](const ListedProblem& had, const Problem& now) {
        return had.problem == now;
    });
    if (unchanged) return;

    // A problem it reported before keeps its arrival; the new ones arrive
    // now, together. Each earlier problem is matched once, so two equal
    // problems stay two.
    const uint64_t now = ++arrivals_;
    std::vector<bool> matched(before.size(), false);
    std::vector<ListedProblem> now_listed;
    now_listed.reserve(problems.size());
    for (Problem& problem : problems) {
        uint64_t arrived = now;
        for (std::size_t i = 0; i < before.size(); ++i) {
            if (!matched[i] && before[i].problem == problem) {
                matched[i] = true;
                arrived = before[i].arrived;
                break;
            }
        }
        now_listed.push_back({.problem = std::move(problem), .source = std::string(source), .arrived = arrived});
    }

    if (found != sources_.end()) found->second = std::move(now_listed);
    else sources_.emplace(std::string(source), std::move(now_listed));
    ++revision_;
}

void ProblemList::clear(std::string_view source) {
    report(source, {});
}

void ProblemList::clear_all() {
    std::lock_guard lock(mutex_);
    if (sources_.empty()) return;
    sources_.clear();
    ++revision_;
}

uint64_t ProblemList::revision() const {
    std::lock_guard lock(mutex_);
    return revision_;
}

ProblemCounts ProblemList::counts() const {
    std::lock_guard lock(mutex_);
    ProblemCounts tally;
    for (const auto& [source, problems] : sources_) {
        for (const ListedProblem& entry : problems) {
            if (entry.problem.severity == ProblemSeverity::Error) ++tally.errors;
            else ++tally.warnings;
        }
    }
    return tally;
}

std::vector<ListedProblem> ProblemList::listed() const {
    std::vector<ListedProblem> all;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [source, problems] : sources_) {
            all.insert(all.end(), problems.begin(), problems.end());
        }
    }
    // Sources come out of the map in key order and each keeps its own, so a
    // stable sort leaves problems that tie in the order they were given.
    std::ranges::stable_sort(all, [](const ListedProblem& a, const ListedProblem& b) {
        if (a.problem.severity != b.problem.severity) return a.problem.severity == ProblemSeverity::Error;
        return a.arrived > b.arrived;
    });
    return all;
}

std::optional<ListedProblem> ProblemList::newest() const {
    std::lock_guard lock(mutex_);
    const ListedProblem* latest = nullptr;
    for (const auto& [source, problems] : sources_) {
        for (const ListedProblem& entry : problems) {
            if (!latest || entry.arrived > latest->arrived) latest = &entry;
        }
    }
    if (!latest) return std::nullopt;
    return *latest;
}

ProblemList& problem_list() {
    static ProblemList list;
    return list;
}

} // namespace ftk
