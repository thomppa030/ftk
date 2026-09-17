#pragma once

#ifndef NDEBUG

#include "platform/file_watcher.hpp"

#include <string>
#include <vector>

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

    /// What changed since the last call. Empty when nothing did.
    struct Changes {
        /// Absolute paths of the `.fjsl` files edited, so a reload can rebuild
        /// only the pipelines that name them.
        std::vector<std::string> fjsl_paths;
        /// A raw GLSL stage or a shared include was recompiled. Which
        /// pipelines that reaches is not knowable from the file — an include
        /// fans out to everything that pulls it in — so it means rebuild all.
        bool rebuild_everything{false};

        [[nodiscard]] bool any() const { return rebuild_everything || !fjsl_paths.empty(); }
    };

    [[nodiscard]] Changes poll();

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
    Changes changes_;
};

} // namespace fjell

#endif // NDEBUG
