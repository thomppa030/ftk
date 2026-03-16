#pragma once

#ifndef NDEBUG

#include <string>

namespace fjell {

class ShaderWatcher {
public:
    ShaderWatcher(const std::string& source_dir, const std::string& output_dir);
    ~ShaderWatcher();

    ShaderWatcher(const ShaderWatcher&) = delete;
    ShaderWatcher& operator=(const ShaderWatcher&) = delete;
    ShaderWatcher(ShaderWatcher&&) = delete;
    ShaderWatcher& operator=(ShaderWatcher&&) = delete;

    [[nodiscard]] bool poll();

private:
    bool recompile(const std::string& filename);

    std::string source_dir_;
    std::string output_dir_;
    int inotify_fd_{-1};
    int watch_fd_{-1};
};

} // namespace fjell

#endif // NDEBUG
