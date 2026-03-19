#ifndef NDEBUG

#include "renderer/shader_watcher.hpp"
#include "core/log.hpp"

#include <sys/inotify.h>
#include <unistd.h>
#include <poll.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace fjell {

ShaderWatcher::ShaderWatcher(const std::string& source_dir, const std::string& output_dir)
    : source_dir_{source_dir}, output_dir_{output_dir} {
    inotify_fd_ = inotify_init1(IN_NONBLOCK);
    if (inotify_fd_ < 0) {
        FJELL_GFX_WARN("Failed to init inotify for shader watching");
        return;
    }

    watch_fd_ = inotify_add_watch(inotify_fd_, source_dir_.c_str(),
                                  IN_CLOSE_WRITE | IN_MOVED_TO);
    if (watch_fd_ < 0) {
        FJELL_GFX_WARN("Failed to watch shader directory: {}", source_dir_);
        close(inotify_fd_);
        inotify_fd_ = -1;
        return;
    }

    FJELL_GFX_INFO("Shader hot-reload watching: {}", source_dir_);
}

ShaderWatcher::~ShaderWatcher() {
    if (watch_fd_ >= 0) {
        inotify_rm_watch(inotify_fd_, watch_fd_);
    }
    if (inotify_fd_ >= 0) {
        close(inotify_fd_);
    }
}

bool ShaderWatcher::poll() {
    if (inotify_fd_ < 0) return false;

    pollfd pfd{};
    pfd.fd = inotify_fd_;
    pfd.events = POLLIN;

    if (::poll(&pfd, 1, 0) <= 0) return false;

    bool changed = false;
    alignas(inotify_event) std::array<char, 4096> buf{};

    auto len = read(inotify_fd_, buf.data(), buf.size());
    if (len <= 0) return false;

    const char* ptr = buf.data();
    while (ptr < buf.data() + len) {
        const auto* event = reinterpret_cast<const inotify_event*>(ptr);

        if (event->len > 0) {
            std::string filename{event->name};
            auto ext = std::filesystem::path(filename).extension().string();
            if (ext == ".vert" || ext == ".frag" || ext == ".comp") {
                FJELL_GFX_INFO("Shader changed: {}", filename);
                if (recompile(filename)) {
                    changed = true;
                }
            }
        }

        ptr += sizeof(inotify_event) + event->len;
    }

    return changed;
}

bool ShaderWatcher::recompile(const std::string& filename) {
    auto src = source_dir_ + "/" + filename;
    auto dst = output_dir_ + "/" + filename + ".spv";

    std::string cmd = "glslc --target-env=vulkan1.3 -I " + source_dir_ + "/include " + src + " -o " + dst + " 2>&1";
    auto* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        FJELL_GFX_ERROR("Failed to run glslc");
        return false;
    }

    std::string output;
    std::array<char, 256> line{};
    while (fgets(line.data(), static_cast<int>(line.size()), pipe)) {
        output += line.data();
    }

    int status = pclose(pipe);
    if (status != 0) {
        FJELL_GFX_ERROR("Shader compilation failed:\n{}", output);
        return false;
    }

    FJELL_GFX_INFO("Shader recompiled: {} -> {}", src, dst);
    return true;
}

} // namespace fjell

#endif // NDEBUG
