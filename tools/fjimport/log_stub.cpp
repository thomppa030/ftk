// Minimal log implementation for the fjimport tool.
// meshlet.cpp includes core/log.hpp which requires these loggers.

#include "core/log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

namespace fjell::log {

static std::shared_ptr<spdlog::logger> s_core;
static std::shared_ptr<spdlog::logger> s_renderer;
static std::shared_ptr<spdlog::logger> s_app;
static std::shared_ptr<ConsoleSink> s_console_sink;

void init() {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sink->set_pattern("%^[%T] [%n] [%l]%$ %v");

    s_core = std::make_shared<spdlog::logger>("CORE", sink);
    s_renderer = std::make_shared<spdlog::logger>("GFX", sink);
    s_app = std::make_shared<spdlog::logger>("APP", sink);

    s_core->set_level(spdlog::level::info);
    s_renderer->set_level(spdlog::level::info);
    s_app->set_level(spdlog::level::info);
}

void shutdown() {
    s_core.reset();
    s_renderer.reset();
    s_app.reset();
}

std::shared_ptr<spdlog::logger>& core() { return s_core; }
std::shared_ptr<spdlog::logger>& renderer() { return s_renderer; }
std::shared_ptr<spdlog::logger>& app() { return s_app; }
std::shared_ptr<ConsoleSink>& console_sink() { return s_console_sink; }

} // namespace fjell::log
