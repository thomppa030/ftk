#pragma once

#ifndef NDEBUG

#include "ftk/platform/file_watcher.hpp"

#include <filesystem>
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
    /// Copy an edited engine shader to its place under `output_dir_`, in
    /// `subdir` of it ("" for the top level). The engine reads its .fjsl
    /// files and includes from the build tree's mirror of
    /// shaders/, which the build keeps current and nothing else does — so
    /// without this a rebuild after an edit reads the text from before it.
    /// False, having logged why, when the copy could not be made; the change
    /// is then not reported, because reloading would only redraw the old
    /// shader and look like the edit did nothing.
    bool mirror(const std::filesystem::path& changed, const std::string& subdir);
    /// Re-establish every watch. FileWatcher::clear() drops all of them at
    /// once, so changing the project's directory means putting the engine's
    /// back too.
    void rewatch();

    std::string source_dir_;
    std::string output_dir_;
    std::string project_dir_;
    ftk::platform::FileWatcher watcher_;
    Changes changes_;
};

} // namespace fjell

#endif // NDEBUG
