#ifndef NDEBUG

#include "renderer/shader_watcher.hpp"
#include "platform/platform.hpp"
#include "core/log.hpp"

#include <filesystem>

namespace fjell {

ShaderWatcher::ShaderWatcher(const std::string& source_dir, const std::string& output_dir)
    : source_dir_{source_dir}, output_dir_{output_dir} {
    rewatch();
}

void ShaderWatcher::watch_project(const std::string& project_shader_dir) {
    if (project_dir_ == project_shader_dir) return;
    project_dir_ = project_shader_dir;
    rewatch();
}

void ShaderWatcher::rewatch() {
    watcher_.clear();

    watcher_.watch(source_dir_, [this](const std::filesystem::path& path) {
        auto ext = path.extension().string();
        if (ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".fjsl") {
            FJELL_GFX_INFO("Shader changed: {}", path.filename().string());
            if (ext == ".fjsl") {
                changes_.fjsl_paths.push_back(path.string());
            } else if (recompile(path.filename().string())) {
                // A raw stage is one of the engine's own, and what reads it is
                // not written down anywhere: rebuild everything.
                changes_.rebuild_everything = true;
            }
        }
    });
    FJELL_GFX_INFO("Shader hot-reload watching: {}", source_dir_);

    if (project_dir_.empty() || !std::filesystem::is_directory(project_dir_)) return;

    watcher_.watch(project_dir_, [this](const std::filesystem::path& path) {
        // A project authors .fjsl and nothing else. The raw GLSL stages are
        // the engine's own, compiled against its include directory, so a
        // project file of that extension would be recompiled with the wrong
        // includes and silently overwrite an engine .spv — skip them.
        if (path.extension() != ".fjsl") return;
        FJELL_GFX_INFO("Project shader changed: {}", path.filename().string());
        changes_.fjsl_paths.push_back(path.string());
    });
    FJELL_GFX_INFO("Shader hot-reload also watching project: {}", project_dir_);
}

ShaderWatcher::Changes ShaderWatcher::poll() {
    changes_ = {};
    (void)watcher_.poll();
    return std::move(changes_);
}

bool ShaderWatcher::recompile(const std::string& filename) {
    auto src = source_dir_ + "/" + filename;
    auto dst = output_dir_ + "/" + filename + ".spv";

    std::string cmd = "glslc --target-env=vulkan1.3 -I " + source_dir_ + "/include " + src + " -o " + dst + " 2>&1";

    std::string output;
    int status = platform::run_command(cmd, output);
    if (status != 0) {
        FJELL_GFX_ERROR("Shader compilation failed:\n{}", output);
        return false;
    }

    FJELL_GFX_INFO("Shader recompiled: {} -> {}", src, dst);
    return true;
}

} // namespace fjell

#endif // NDEBUG
