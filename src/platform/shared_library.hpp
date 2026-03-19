#pragma once

#include <filesystem>
#include <string>

namespace fjell::platform {

/// Cross-platform dynamic library loader.
///
/// Linux/macOS: dlopen/dlsym. Windows: LoadLibrary/GetProcAddress (future).
class SharedLibrary {
public:
    SharedLibrary() = default;
    ~SharedLibrary();

    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;
    SharedLibrary(SharedLibrary&& other) noexcept;
    SharedLibrary& operator=(SharedLibrary&& other) noexcept;

    /// Load a shared library from the given path.
    [[nodiscard]] bool load(const std::filesystem::path& path);

    /// Unload the library.
    void unload();

    /// Resolve a symbol by name. Returns nullptr if not found.
    [[nodiscard]] void* symbol(const char* name) const;

    /// Typed symbol lookup.
    template<typename T>
    [[nodiscard]] T symbol_as(const char* name) const {
        return reinterpret_cast<T>(symbol(name));
    }

    [[nodiscard]] bool is_loaded() const { return handle_ != nullptr; }

    /// Last error message from load/symbol failure.
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    void* handle_{nullptr};
    std::string error_;
};

} // namespace fjell::platform
