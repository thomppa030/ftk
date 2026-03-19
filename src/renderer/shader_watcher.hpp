#pragma once

#ifndef NDEBUG

#include "platform/file_watcher.hpp"

#include <string>

namespace fjell {

class ShaderWatcher {
public:
    ShaderWatcher(const std::string& source_dir, const std::string& output_dir);
    ~ShaderWatcher() = default;

    ShaderWatcher(const ShaderWatcher&) = delete;
    ShaderWatcher& operator=(const ShaderWatcher&) = delete;
    ShaderWatcher(ShaderWatcher&&) = delete;
    ShaderWatcher& operator=(ShaderWatcher&&) = delete;

    [[nodiscard]] bool poll();

private:
    bool recompile(const std::string& filename);

    std::string source_dir_;
    std::string output_dir_;
    platform::FileWatcher watcher_;
    bool changed_{false};
};

} // namespace fjell

#endif // NDEBUG
