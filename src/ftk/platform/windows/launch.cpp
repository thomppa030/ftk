#include "ftk/platform/windows/launch.hpp"

#include "ftk/platform/detail/command_line.hpp"
#include "ftk/platform/platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <filesystem>
#include <optional>
#include <vector>

namespace ftk::platform::detail {

namespace {

// The system's own cmd.exe, never one found anywhere else; nothing when
// Windows won't say where its folder is.
std::optional<std::wstring> system_cmd() {
    std::array<wchar_t, MAX_PATH> folder{};
    const UINT length = GetSystemDirectoryW(folder.data(), static_cast<UINT>(folder.size()));
    if (length == 0 || length >= folder.size()) return std::nullopt;
    return std::wstring(folder.data(), length) + L"\\cmd.exe";
}

} // namespace

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
    // The file find_program() finds, so a name without its extension starts
    // what cmd would start by it, a batch file too. Where it finds none,
    // Windows looks itself, and says what went wrong.
    const std::optional<std::filesystem::path> found = find_program(args.front());
    const std::wstring program = found ? found->wstring() : wide_args.front();
    if (!is_batch_file<wchar_t>(program)) {
        return Launch{.program = found ? program : std::wstring{}, .line = command_line<wchar_t>(wide_args)};
    }
    if (!found) return make_error("there is no such batch file");
    // A batch file is run by cmd.exe, its arguments written for cmd to hand
    // on untouched.
    std::optional<std::wstring> line =
        batch_command_line<wchar_t>(program, std::span<const std::wstring>(wide_args).subspan(1));
    if (!line) return make_error("a batch file's name can't hold a quote, nor its arguments a line break");
    std::optional<std::wstring> cmd = system_cmd();
    if (!cmd) return make_error("Windows won't say where cmd.exe is, which runs batch files");
    return Launch{.program = std::move(*cmd), .line = std::move(*line)};
}

} // namespace ftk::platform::detail
