#include "core/log.hpp"
#include "ui/console.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdlib>
#include <string_view>
#include <vector>

namespace fjell::log {

static std::shared_ptr<spdlog::logger> s_core;
static std::shared_ptr<spdlog::logger> s_renderer;
static std::shared_ptr<spdlog::logger> s_app;
static std::shared_ptr<ConsoleSink> s_console_sink;

// Debug and trace lines are diagnostics for chasing a specific problem, so
// they stay off until asked for. FJELL_LOG_LEVEL raises (or lowers) the
// floor for one run without a rebuild.
static spdlog::level::level_enum initial_level() {
    if (const char* env = std::getenv("FJELL_LOG_LEVEL")) {
        std::string_view v = env;
        if (v == "trace") return spdlog::level::trace;
        if (v == "debug") return spdlog::level::debug;
        if (v == "info") return spdlog::level::info;
        if (v == "warn") return spdlog::level::warn;
        if (v == "error") return spdlog::level::err;
    }
    return spdlog::level::info;
}

void init() {
    auto terminal_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    terminal_sink->set_pattern("%^[%T.%e] [%n] [%l]%$ %v");

    s_console_sink = std::make_shared<ConsoleSink>();
    s_console_sink->set_pattern("[%T] [%n] [%l] %v");

    std::vector<spdlog::sink_ptr> sinks = {terminal_sink, s_console_sink};

    s_core = std::make_shared<spdlog::logger>("CORE", sinks.begin(), sinks.end());
    s_renderer = std::make_shared<spdlog::logger>("GFX", sinks.begin(), sinks.end());
    s_app = std::make_shared<spdlog::logger>("APP", sinks.begin(), sinks.end());

    const auto level = initial_level();
    s_core->set_level(level);
    s_renderer->set_level(level);
    s_app->set_level(level);

    spdlog::register_logger(s_core);
    spdlog::register_logger(s_renderer);
    spdlog::register_logger(s_app);

    s_core->flush_on(spdlog::level::warn);
    s_renderer->flush_on(spdlog::level::warn);
    s_app->flush_on(spdlog::level::warn);
}

void shutdown() {
    spdlog::shutdown();
    s_core.reset();
    s_renderer.reset();
    s_app.reset();
    s_console_sink.reset();
}

std::shared_ptr<spdlog::logger>& core() { return s_core; }
std::shared_ptr<spdlog::logger>& renderer() { return s_renderer; }
std::shared_ptr<spdlog::logger>& app() { return s_app; }
std::shared_ptr<ConsoleSink>& console_sink() { return s_console_sink; }

} // namespace fjell::log
