#pragma once

// How the Windows backend starts a program: what every starter hands
// CreateProcessW for a program and its arguments, so each starts the same
// program the same way.

#include "ftk/base/result.hpp"

#include <span>
#include <string>
#include <string_view>

namespace ftk::platform::detail {

/// What CreateProcessW is given to start a program.
struct Launch {
    /// The file Windows runs; empty to have it find the command line's first
    /// word itself.
    std::wstring program;
    /// The command line it runs it with, its first word the program, for
    /// the program to read its arguments back from.
    std::wstring line;
};

/// `args`, the program first, ready for CreateProcessW: a batch file run by
/// the system's cmd.exe with its arguments written for cmd, and all in
/// UTF-16, so that every letter of an argument reaches the program as
/// written.
/// @return what to start, or why it can't be: an argument no quoting
///         carries past cmd
[[nodiscard]] Result<Launch> launch(std::span<const std::string> args);

/// The UTF-8 `text` in Windows' UTF-16.
[[nodiscard]] std::wstring wide(std::string_view text);

} // namespace ftk::platform::detail
