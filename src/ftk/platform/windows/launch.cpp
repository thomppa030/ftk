#include "ftk/platform/windows/launch.hpp"

#include "ftk/platform/detail/command_line.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <vector>

namespace ftk::platform::detail {

std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

Result<Launch> launch(std::span<const std::string> args) {
    if (args.empty()) return make_error("there is no program to run");
    std::vector<std::wstring> wide_args;
    wide_args.reserve(args.size());
    for (const std::string& arg : args) wide_args.push_back(wide(arg));
    return Launch{.program = {}, .line = command_line<wchar_t>(wide_args)};
}

} // namespace ftk::platform::detail
