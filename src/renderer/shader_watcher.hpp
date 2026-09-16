#pragma once

#ifndef NDEBUG

#include "platform/file_watcher.hpp"

#include <string>

namespace fjell {

class ShaderWatcher {
public:
    ShaderWatcher(const std::string& source_dir, const std::string& output_dir);
    ~ShaderWatcher() = default;

    /// Also watch a game project's shaders, so editing one reloads it the way
    /// editing an engine shader does. Only `.fjsl` is picked up there: raw
    /// GLSL stages belong to the engine and are compiled against its include
    /// directory, which a project's are not.
    ///
    /// Call again when a different project opens; the previous project's
    /// watch is dropped. An empty path just drops it.
    void watch_project(const std::string& project_shader_dir);

    ShaderWatcher(const ShaderWatcher&) = delete;
    ShaderWatcher& operator=(const ShaderWatcher&) = delete;
    ShaderWatcher(ShaderWatcher&&) = delete;
    ShaderWatcher& operator=(ShaderWatcher&&) = delete;

    [[nodiscard]] bool poll();

private:
    bool recompile(const std::string& filename);
    /// Re-establish every watch. FileWatcher::clear() drops all of them at
    /// once, so changing the project's directory means putting the engine's
    /// back too.
    void rewatch();

    std::string source_dir_;
    std::string output_dir_;
    std::string project_dir_;
    platform::FileWatcher watcher_;
    bool changed_{false};
};

} // namespace fjell

#endif // NDEBUG
