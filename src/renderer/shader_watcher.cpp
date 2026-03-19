#ifndef NDEBUG

#include "renderer/shader_watcher.hpp"
#include "platform/platform.hpp"
#include "core/log.hpp"

#include <filesystem>

namespace fjell {

ShaderWatcher::ShaderWatcher(const std::string& source_dir, const std::string& output_dir)
    : source_dir_{source_dir}, output_dir_{output_dir} {
    watcher_.watch(source_dir_, [this](const std::filesystem::path& path) {
        auto ext = path.extension().string();
        if (ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".fjsl") {
            FJELL_GFX_INFO("Shader changed: {}", path.filename().string());
            if (ext == ".fjsl") {
                changed_ = true;
            } else if (recompile(path.filename().string())) {
                changed_ = true;
            }
        }
    });

    FJELL_GFX_INFO("Shader hot-reload watching: {}", source_dir_);
}

bool ShaderWatcher::poll() {
    changed_ = false;
    (void)watcher_.poll();
    return changed_;
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
