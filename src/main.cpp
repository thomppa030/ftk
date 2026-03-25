#include "core/engine.hpp"
#include "core/log.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <Windows.h>
#endif

static std::string safe_what(const std::exception& e) {
    const char* msg = e.what();
    if (!msg) return "(null)";

#ifdef _WIN32
    // Try to convert from the system locale to UTF-8
    int wlen = MultiByteToWideChar(CP_ACP, 0, msg, -1, nullptr, 0);
    if (wlen > 0) {
        std::wstring wide(wlen, L'\0');
        MultiByteToWideChar(CP_ACP, 0, msg, -1, wide.data(), wlen);
        int u8len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (u8len > 0) {
            std::string utf8(u8len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), u8len, nullptr, nullptr);
            return utf8;
        }
    }
#endif
    return msg;
}

int main(int argc, char* argv[]) {
    fjell::log::init();

    std::filesystem::path project_path;
    bool play_immediate = false;
    std::string scene_override;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--play") == 0) {
            play_immediate = true;
        } else if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            scene_override = argv[++i];
        } else {
            project_path = argv[i];
        }
    }

    try {
        fjell::Engine engine{"Fjell Engine", 2560, 1360, project_path};
        if (!scene_override.empty()) engine.set_scene_override(scene_override);
        if (play_immediate) engine.set_play_immediate(true);
        engine.run();
    } catch (const std::exception& e) {
        FJELL_CORE_CRITICAL("Fatal: {}", safe_what(e));
        fjell::log::shutdown();
        return EXIT_FAILURE;
    }

    fjell::log::shutdown();
    return EXIT_SUCCESS;
}
