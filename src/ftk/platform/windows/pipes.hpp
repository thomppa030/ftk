#pragma once

// What the Windows backend does with a pipe or a stream by its handle:
// reading what has come, and writing all of a text.

#include "ftk/base/result.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace ftk::platform::detail {

/// What Windows says `error` is, in UTF-8.
[[nodiscard]] std::string reason(DWORD error);

/// Waits for what comes on `handle` and reads it into `into`: how many
/// bytes, none at the end of the stream (its writer closed it) or from no
/// handle. `what` names the stream in an error.
[[nodiscard]] Result<std::size_t> read_some(HANDLE handle, std::span<char> into, std::string_view what);

/// Writes all of `bytes` to `handle`, waiting while its pipe is full.
[[nodiscard]] Result<> write_all(HANDLE handle, std::string_view bytes, std::string_view what);

} // namespace ftk::platform::detail
