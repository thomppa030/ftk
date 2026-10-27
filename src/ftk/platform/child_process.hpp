#pragma once

// A program run as a child of this one and talked to through its standard
// streams, as a language server is: what is written goes to its input, and
// what it writes back is read from its output. A read waits for the child,
// so it is made on a thread of the caller's own; a write waits only while
// the pipe is full. What it writes to its standard error is let go, shown
// where this program's goes, or kept to be read as well.
//
//     auto child = platform::ChildProcess::start(std::vector<std::string>{"clangd"});
//     if (!child) return report(child.error());
//     (void)(*child)->write(request);
//     // on a reader thread:
//     std::array<char, 4096> buffer;
//     while (auto got = (*child)->read(buffer)) { if (*got == 0) break; ... }

#include "ftk/base/result.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ftk::platform {

/// Where what a child writes to its standard error goes.
enum class ChildErrors {
    /// Nowhere.
    Discarded,
    /// Where this program's own go.
    Shown,
    /// Into a pipe of its own, read with read_errors(). Whoever keeps them
    /// reads them: once the pipe is full, the child waits.
    Kept,
};

struct ChildOptions {
    /// The folder it starts in; empty is this program's own.
    std::filesystem::path folder{};
    ChildErrors errors{ChildErrors::Discarded};
};

class ChildProcess {
public:
    /// Starts the program `args[0]`, a path or a name looked for along the
    /// PATH, with the rest of `args` as its arguments, each reaching it as
    /// it is given. On Windows a batch file is run by cmd.exe, which hands
    /// its arguments on as given; it can't be given a line break.
    /// @return the child, or why it couldn't be run (no such program, no
    ///         such folder, no pipes left)
    [[nodiscard]] static Result<std::unique_ptr<ChildProcess>> start(std::span<const std::string> args,
                                                                     const ChildOptions& options = {});

    /// Ends it if it still runs and waits for it. A thread reading from it
    /// is done before it goes.
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;

    /// Writes all of `bytes` to its input, waiting while the pipe is full.
    /// From one thread at a time, as close_input() is.
    /// @return nothing, or an error once its input is closed or it has ended
    [[nodiscard]] Result<> write(std::string_view bytes);

    /// Closes its input, so it reads to the end of it.
    void close_input();

    /// Waits for it to write to its output and reads what it wrote into
    /// `into`. From one thread at a time.
    /// @return how many bytes, none once its output has ended (it closed
    ///         it, or ended), or why it couldn't be read
    [[nodiscard]] Result<std::size_t> read(std::span<char> into);

    /// The same for its standard error, when it is kept (ChildErrors::Kept);
    /// none at once otherwise.
    [[nodiscard]] Result<std::size_t> read_errors(std::span<char> into);

    /// Its exit code once it has ended, -1 when it was ended from outside;
    /// nothing while it runs.
    [[nodiscard]] std::optional<int> exit_code();

    /// Waits for it to end. @return its exit code, as exit_code() gives it
    int wait();

    /// Ends it at once, if it still runs. A read waiting on it then finds
    /// the end of its output.
    void kill();

private:
    struct Impl;
    explicit ChildProcess(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace ftk::platform
