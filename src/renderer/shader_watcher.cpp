#ifndef NDEBUG

#include "renderer/shader_watcher.hpp"
#include "ftk/platform/platform.hpp"
#include "ftk/base/log.hpp"

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
            FTK_GFX_INFO("Shader changed: {}", path.filename().string());
            if (ext == ".fjsl") {
                if (mirror(path, "")) changes_.fjsl_paths.push_back(path.string());
            } else if (recompile(path.filename().string())) {
                // A raw stage is one of the engine's own, and what reads it is
                // not written down anywhere: rebuild everything.
                changes_.rebuild_everything = true;
            }
        }
    });
    FTK_GFX_INFO("Shader hot-reload watching: {}", source_dir_);

    // Includes sit in their own directory, with folders of their own inside
    // it (sky/, tonemap/), and inotify does not recurse, so each needs a
    // watch of its own; a folder made while the engine runs is watched from
    // the next rewatch. What an include reaches is not written down anywhere
    // — the templates every .fjsl is generated against live here — so any
    // edit means rebuild everything.
    const std::filesystem::path include_dir = std::filesystem::path(source_dir_) / "include";
    if (std::filesystem::is_directory(include_dir)) {
        auto watch_includes = [this, &include_dir](const std::filesystem::path& dir) {
            const std::filesystem::path relative = dir.lexically_relative(include_dir);
            const std::string subdir = relative == "."
                ? std::string("include")
                : (std::filesystem::path("include") / relative).generic_string();
            watcher_.watch(dir, [this, subdir](const std::filesystem::path& path) {
                if (path.extension() != ".glsl") return;
                FTK_GFX_INFO("Shader include changed: {}", path.filename().string());
                if (mirror(path, subdir)) changes_.rebuild_everything = true;
            });
        };
        watch_includes(include_dir);
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(include_dir, ec)) {
            if (entry.is_directory()) watch_includes(entry.path());
        }
        FTK_GFX_INFO("Shader hot-reload watching includes: {}", include_dir.string());
    }

    if (project_dir_.empty() || !std::filesystem::is_directory(project_dir_)) return;

    watcher_.watch(project_dir_, [this](const std::filesystem::path& path) {
        // A project authors .fjsl and nothing else. The raw GLSL stages are
        // the engine's own, compiled against its include directory, so a
        // project file of that extension would be recompiled with the wrong
        // includes and silently overwrite an engine .spv — skip them.
        if (path.extension() != ".fjsl") return;
        FTK_GFX_INFO("Project shader changed: {}", path.filename().string());
        changes_.fjsl_paths.push_back(path.string());
    });
    FTK_GFX_INFO("Shader hot-reload also watching project: {}", project_dir_);
}

ShaderWatcher::Changes ShaderWatcher::poll() {
    changes_ = {};
    (void)watcher_.poll();
    return std::move(changes_);
}

bool ShaderWatcher::mirror(const std::filesystem::path& changed, const std::string& subdir) {
    const std::filesystem::path dst =
        std::filesystem::path(output_dir_) / subdir / changed.filename();

    std::error_code ec;
    // An install that runs from its sources has nothing to mirror, and
    // copying a file onto itself is an error rather than a no-op.
    if (std::filesystem::equivalent(changed, dst, ec)) return true;

    std::filesystem::create_directories(dst.parent_path(), ec);
    std::filesystem::copy_file(changed, dst,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        FTK_GFX_ERROR("Shader hot-reload could not copy {} to {}: {}",
                        changed.string(), dst.string(), ec.message());
        return false;
    }
    return true;
}

bool ShaderWatcher::recompile(const std::string& filename) {
    auto src = source_dir_ + "/" + filename;
    auto dst = output_dir_ + "/" + filename + ".spv";

    std::string cmd = "glslc --target-env=vulkan1.3 -I " + source_dir_ + "/include " + src + " -o " + dst + " 2>&1";

    std::string output;
    int status = ftk::platform::run_command(cmd, output);
    if (status != 0) {
        FTK_GFX_ERROR("Shader compilation failed:\n{}", output);
        return false;
    }

    FTK_GFX_INFO("Shader recompiled: {} -> {}", src, dst);
    return true;
}

} // namespace fjell

#endif // NDEBUG
