#include "environment/environment_scenario.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace fjell {

namespace {

/// Whether a whole token is a number, so a misspelled value is an error
/// rather than a silent zero.
bool parse_float(std::string_view token, float& out) {
    // from_chars over floats is not in GCC 13's libstdc++ for every type, so
    // the check is done by hand around strtof.
    const std::string text(token);
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') return false;
    out = value;
    return true;
}

bool parse_uint(std::string_view token, uint32_t& out) {
    const char* begin = token.data();
    const char* end = begin + token.size();
    uint32_t value = 0;
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) return false;
    out = value;
    return true;
}

std::string line_error(const std::string& path, size_t line, const std::string& what) {
    return path + ":" + std::to_string(line) + ": " + what;
}

} // namespace

Result<EnvironmentScenario> load_environment_scenario(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return make_error("Cannot open scenario file: " + path);
    }

    EnvironmentScenario scenario;
    std::string text;
    size_t line_number = 0;

    while (std::getline(file, text)) {
        ++line_number;
        if (const auto hash = text.find('#'); hash != std::string::npos) {
            text.erase(hash);
        }
        std::istringstream tokens(text);
        std::string time_token;
        if (!(tokens >> time_token)) continue;   // blank or comment-only

        EnvironmentScenario::Step step;
        if (!parse_float(time_token, step.time)) {
            return make_error(line_error(path, line_number,
                                         "'" + time_token + "' is not a time in seconds"));
        }
        if (step.time < 0.0f) {
            return make_error(line_error(path, line_number, "a step cannot be before the start"));
        }

        std::string command;
        if (!(tokens >> command)) {
            return make_error(line_error(path, line_number, "a time with no command"));
        }

        if (command == "quit") {
            step.quit = true;
        } else if (command == "hour") {
            std::string value;
            float hour = 0.0f;
            if (!(tokens >> value) || !parse_float(value, hour)) {
                return make_error(line_error(path, line_number, "hour needs a number, 0 to 24"));
            }
            step.change.hour = hour;
        } else if (command == "day-length") {
            std::string value;
            float seconds = 0.0f;
            if (!(tokens >> value) || !parse_float(value, seconds)) {
                return make_error(line_error(path, line_number,
                                             "day-length needs seconds, 0 to freeze the clock"));
            }
            step.change.day_length = seconds;
        } else if (command == "weather") {
            std::string event;
            if (!(tokens >> event)) {
                return make_error(line_error(path, line_number,
                                             "weather needs a .fjweather path"));
            }
            step.change.weather = event;
            // The phase is optional and defaults to the approach, which is
            // weather arriving normally.
            if (std::string phase_name; tokens >> phase_name) {
                const auto phase = weather_phase_from_name(phase_name);
                if (!phase) {
                    return make_error(line_error(path, line_number,
                                                 "'" + phase_name + "' is not a phase "
                                                 "(approach, onset, sustain, tail)"));
                }
                step.change.weather_phase = *phase;
            }
        } else if (command == "wetness" || command == "snow") {
            std::string value;
            float amount = 0.0f;
            if (!(tokens >> value) || !parse_float(value, amount)) {
                return make_error(line_error(path, line_number,
                                             command + " needs a number, 0 to 1"));
            }
            if (command == "wetness") step.change.wetness = amount;
            else step.change.snow = amount;
        } else if (command == "hold-weather") {
            step.change.hold_weather = true;
        } else if (command == "release-weather") {
            step.change.hold_weather = false;
        } else if (command == "seed") {
            std::string value;
            uint32_t seed = 0;
            if (!(tokens >> value) || !parse_uint(value, seed)) {
                return make_error(line_error(path, line_number, "seed needs a whole number"));
            }
            step.change.seed = seed;
        } else {
            return make_error(line_error(path, line_number, "'" + command + "' is not a command "
                                         "(hour, day-length, weather, wetness, snow, "
                                         "hold-weather, release-weather, seed, quit)"));
        }

        scenario.steps.push_back(std::move(step));
    }

    // Authored order is the reader's business, not the player's; a stable
    // sort keeps two steps at the same second in the order they were
    // written, which is how `hour` then `weather` on one second behaves as
    // the file reads.
    std::stable_sort(scenario.steps.begin(), scenario.steps.end(),
                     [](const EnvironmentScenario::Step& a,
                        const EnvironmentScenario::Step& b) { return a.time < b.time; });
    return scenario;
}

} // namespace fjell
