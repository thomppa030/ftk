#pragma once

// What the Linux backend does with a pipe or a stream by its file
// descriptor: reading what has come, and writing all of a text without the
// signal a pipe no one reads raises, which would end the program.

#include "ftk/base/result.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace ftk::platform::detail {

/// Waits for what comes on `fd` and reads it into `into`: how many bytes,
/// none at the end of the stream or from an `fd` already closed (below
/// zero). `what` names the stream in an error.
[[nodiscard]] Result<std::size_t> read_some(int fd, std::span<char> into, std::string_view what);

/// Writes all of `bytes` to `fd`, waiting while its pipe is full; an error,
/// not SIGPIPE, when no one reads it any more.
[[nodiscard]] Result<> write_all(int fd, std::string_view bytes, std::string_view what);

} // namespace ftk::platform::detail
