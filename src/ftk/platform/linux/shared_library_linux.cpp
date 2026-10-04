#include "ftk/platform/shared_library.hpp"

#include <dlfcn.h>

#include <utility>

namespace ftk::platform {

SharedLibrary::~SharedLibrary() {
    unload();
}

SharedLibrary::SharedLibrary(SharedLibrary&& other) noexcept
    : handle_{std::exchange(other.handle_, nullptr)}
    , kept_{std::exchange(other.kept_, false)}
    , error_{std::move(other.error_)} {}

SharedLibrary& SharedLibrary::operator=(SharedLibrary&& other) noexcept {
    if (this != &other) {
        unload();
        handle_ = std::exchange(other.handle_, nullptr);
        kept_ = std::exchange(other.kept_, false);
        error_ = std::move(other.error_);
    }
    return *this;
}

bool SharedLibrary::load(const std::filesystem::path& path) {
    unload();
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
        const char* err = dlerror();
        error_ = err ? err : "unknown dlopen error";
        return false;
    }
    error_.clear();
    return true;
}

void SharedLibrary::unload() {
    if (handle_) {
        if (!kept_) dlclose(handle_);
        handle_ = nullptr;
        kept_ = false;
    }
}

void* SharedLibrary::symbol(const char* name) const {
    if (!handle_) return nullptr;
    dlerror(); // clear previous errors
    void* sym = dlsym(handle_, name);
    const char* err = dlerror();
    if (err) {
        const_cast<SharedLibrary*>(this)->error_ = err;
        return nullptr;
    }
    return sym;
}

} // namespace ftk::platform
