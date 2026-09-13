#include "platform/shared_library.hpp"

#ifndef _WIN32
#error "This file should only be compiled on Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <utility>

namespace fjell::platform {

SharedLibrary::~SharedLibrary() {
    unload();
}

SharedLibrary::SharedLibrary(SharedLibrary&& other) noexcept
    : handle_{std::exchange(other.handle_, nullptr)}
    , error_{std::move(other.error_)} {}

SharedLibrary& SharedLibrary::operator=(SharedLibrary&& other) noexcept {
    if (this != &other) {
        unload();
        handle_ = std::exchange(other.handle_, nullptr);
        error_ = std::move(other.error_);
    }
    return *this;
}

bool SharedLibrary::load(const std::filesystem::path& path) {
    unload();
    HMODULE h = LoadLibraryW(path.c_str());
    if (!h) {
        DWORD err = GetLastError();
        error_ = "LoadLibrary failed (error " + std::to_string(err) + ")";
        return false;
    }
    handle_ = static_cast<void*>(h);
    error_.clear();
    return true;
}

void SharedLibrary::unload() {
    if (handle_) {
        FreeLibrary(static_cast<HMODULE>(handle_));
        handle_ = nullptr;
    }
}

void* SharedLibrary::symbol(const char* name) const {
    if (!handle_) return nullptr;
    FARPROC proc = GetProcAddress(static_cast<HMODULE>(handle_), name);
    if (!proc) {
        DWORD err = GetLastError();
        const_cast<SharedLibrary*>(this)->error_ =
            "GetProcAddress failed for '" + std::string(name) + "' (error " + std::to_string(err) + ")";
        return nullptr;
    }
    return reinterpret_cast<void*>(proc);
}

} // namespace fjell::platform
