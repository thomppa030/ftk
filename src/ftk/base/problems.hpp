#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What is broken in what the program has open, as the systems that find it
// report it: a shader that fails to compile, a sheet with an unknown
// property, a texture that can't be baked. A source reports under a key of
// its own the whole set it has found, which replaces what it reported
// before, so a problem goes away when its source looks again and finds it
// fixed. The editor lists them and counts them; nothing polls a source.
//
//     ftk::problem_list().report("shader:shaders/sea.fjsl", {{
//         .severity = ftk::ProblemSeverity::Error,
//         .message = "'foam_bias' : undeclared identifier",
//         .meanwhile = "It draws with the last version that compiled",
//         .file = "shaders/sea.fjsl",
//         .line = 41,
//     }});

namespace ftk {

enum class ProblemSeverity { Error, Warning };

/// One thing that is wrong, and where.
struct Problem {
    ProblemSeverity severity{ProblemSeverity::Error};
    /// What is wrong, in one sentence.
    std::string message{};
    /// What happens until it is fixed ("It draws with the last version that
    /// compiled"), or empty.
    std::string meanwhile{};
    /// The file it is in, as a path the program can open; empty for none.
    std::string file{};
    /// 1-based; 0 for the file as a whole.
    int line{0};
    /// 1-based; 0 for the line as a whole.
    int column{0};

    bool operator==(const Problem&) const = default;
};

/// Where `problem` is as a list shows it: the file's name, then the line and
/// column ("hud.fjss:12:5"); empty for a problem in no file.
[[nodiscard]] std::string short_where(const Problem& problem);

/// The same with the file's whole path, to tell two files of one name apart
/// or to copy.
[[nodiscard]] std::string full_where(const Problem& problem);

/// A problem on the list: who reported it and when it arrived.
struct ListedProblem {
    Problem problem{};
    std::string source{};
    /// Larger is newer. Problems reported together arrive together, and one
    /// reported again unchanged keeps the arrival it had.
    uint64_t arrived{0};
};

struct ProblemCounts {
    int errors{0};
    int warnings{0};

    [[nodiscard]] int total() const { return errors + warnings; }
};

/// Every problem the sources have reported and not yet taken back. Safe to
/// report into from any thread.
class ProblemList {
public:
    /// Replaces what `source` reported with `problems`, in their order. An
    /// empty set takes them all back.
    void report(std::string_view source, std::vector<Problem> problems);

    /// Takes back what `source` reported.
    void clear(std::string_view source);

    /// Takes back everything: the program closed what it had open.
    void clear_all();

    /// Moves whenever the list changes, and only then: a source reporting
    /// the set it reported before changes nothing.
    [[nodiscard]] uint64_t revision() const;

    [[nodiscard]] ProblemCounts counts() const;

    /// Every problem: errors first, the newest first within each severity,
    /// a source's in the order it gave them.
    [[nodiscard]] std::vector<ListedProblem> listed() const;

    /// The problem that arrived last, the first of its set when several
    /// arrived together. None when the list is empty.
    [[nodiscard]] std::optional<ListedProblem> newest() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<ListedProblem>, std::less<>> sources_;
    uint64_t arrivals_{0};
    uint64_t revision_{0};
};

/// The program's list, one per process as there is one log.
[[nodiscard]] ProblemList& problem_list();

} // namespace ftk
