#include "ftk/platform/windows/pipes.hpp"

#include <format>

namespace ftk::platform::detail {

std::string reason(DWORD error) {
    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
        reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string out = std::format("error {}", error);
    if (length > 0 && text != nullptr) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        out.assign(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(), size, nullptr, nullptr);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == '.')) out.pop_back();
    }
    if (text != nullptr) LocalFree(text);
    return out;
}

Result<std::size_t> read_some(HANDLE handle, std::span<char> into, std::string_view what) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return std::size_t{0};
    DWORD got = 0;
    if (ReadFile(handle, into.data(), static_cast<DWORD>(into.size()), &got, nullptr)) return std::size_t{got};
    const DWORD error = GetLastError();
    // Its writer closed it: the end of what comes.
    if (error == ERROR_BROKEN_PIPE) return std::size_t{0};
    return make_error(std::format("can't read from {}: {}", what, reason(error)));
}

Result<> write_all(HANDLE handle, std::string_view bytes, std::string_view what) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return make_error(std::format("{} is closed", what));
    while (!bytes.empty()) {
        DWORD wrote = 0;
        if (!WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr)) {
            return make_error(std::format("can't write to {}: {}", what, reason(GetLastError())));
        }
        bytes.remove_prefix(wrote);
    }
    return {};
}

} // namespace ftk::platform::detail
