#include "core/engine.hpp"
#include "core/log.hpp"
#include "environment/environment_overrides.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
// Without NOMINMAX, Windows.h defines min/max as macros that break std::min
// and std::max in anything included after it. Guarded because targets that
// pull in windows.h through another header define these on the command line.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
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
    std::string create_name;
    std::string scenario_path;
    fjell::EnvironmentOverrides environment;

    // A flag that takes a value and is given none is a mistake worth
    // reporting: silently treating "--hour" as a project path would open the
    // hub and leave the run measuring nothing.
    bool bad_usage = false;
    auto value_for = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) {
            FJELL_CORE_ERROR("{} needs a value", flag);
            bad_usage = true;
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--play") == 0) {
            play_immediate = true;
        } else if (std::strcmp(argv[i], "--scene") == 0) {
            if (const char* v = value_for(i, "--scene")) scene_override = v;
        } else if (std::strcmp(argv[i], "--create-project") == 0) {
            if (const char* v = value_for(i, "--create-project")) create_name = v;
        } else if (std::strcmp(argv[i], "--hour") == 0) {
            if (const char* v = value_for(i, "--hour")) environment.hour = std::strtof(v, nullptr);
        } else if (std::strcmp(argv[i], "--day-length") == 0) {
            if (const char* v = value_for(i, "--day-length")) {
                environment.day_length = std::strtof(v, nullptr);
            }
        } else if (std::strcmp(argv[i], "--weather") == 0) {
            if (const char* v = value_for(i, "--weather")) environment.weather = v;
        } else if (std::strcmp(argv[i], "--weather-phase") == 0) {
            if (const char* v = value_for(i, "--weather-phase")) {
                if (auto phase = fjell::weather_phase_from_name(v)) {
                    environment.weather_phase = *phase;
                } else {
                    FJELL_CORE_ERROR("--weather-phase: '{}' is not a phase "
                                     "(approach, onset, sustain, tail)", v);
                    bad_usage = true;
                }
            }
        } else if (std::strcmp(argv[i], "--wetness") == 0) {
            if (const char* v = value_for(i, "--wetness")) {
                environment.wetness = std::strtof(v, nullptr);
            }
        } else if (std::strcmp(argv[i], "--snow") == 0) {
            if (const char* v = value_for(i, "--snow")) environment.snow = std::strtof(v, nullptr);
        } else if (std::strcmp(argv[i], "--hold-weather") == 0) {
            environment.hold_weather = true;
        } else if (std::strcmp(argv[i], "--seed") == 0) {
            if (const char* v = value_for(i, "--seed")) {
                environment.seed = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
            }
        } else if (std::strcmp(argv[i], "--scenario") == 0) {
            if (const char* v = value_for(i, "--scenario")) scenario_path = v;
        } else {
            project_path = argv[i];
        }
    }

    if (bad_usage) {
        fjell::log::shutdown();
        return EXIT_FAILURE;
    }

    bool failed = false;
    try {
        fjell::Engine engine{"Fjell Engine", 2560, 1360, project_path, create_name};
        if (!scene_override.empty()) engine.set_scene_override(scene_override);
        if (play_immediate) engine.set_play_immediate(true);
        if (environment.any()) engine.set_environment_overrides(environment);
        if (!scenario_path.empty()) {
            // A scenario that will not load means the run measures something
            // other than what was asked for, so it does not start.
            if (auto loaded = engine.set_scenario(scenario_path); !loaded) {
                FJELL_CORE_ERROR("{}", loaded.error());
                failed = true;
            }
        }
        // Leaving the scope is what tears the engine down, and its destructor
        // logs. Returning from inside it would shut the log down first and
        // leave that logging through a destroyed sink.
        if (!failed) engine.run();
    } catch (const std::exception& e) {
        FJELL_CORE_CRITICAL("Fatal: {}", safe_what(e));
        fjell::log::shutdown();
        return EXIT_FAILURE;
    }

    fjell::log::shutdown();
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
