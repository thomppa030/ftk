#include "core/log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <vector>

namespace fjell::log {

static std::shared_ptr<spdlog::logger> s_core;
static std::shared_ptr<spdlog::logger> s_renderer;
static std::shared_ptr<spdlog::logger> s_app;

void init() {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("%^[%T.%e] [%n] [%l]%$ %v");

    s_core = std::make_shared<spdlog::logger>("CORE", console_sink);
    s_renderer = std::make_shared<spdlog::logger>("GFX", console_sink);
    s_app = std::make_shared<spdlog::logger>("APP", console_sink);

#ifdef NDEBUG
    s_core->set_level(spdlog::level::info);
    s_renderer->set_level(spdlog::level::info);
    s_app->set_level(spdlog::level::info);
#else
    s_core->set_level(spdlog::level::trace);
    s_renderer->set_level(spdlog::level::trace);
    s_app->set_level(spdlog::level::trace);
#endif

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
}

std::shared_ptr<spdlog::logger>& core() { return s_core; }
std::shared_ptr<spdlog::logger>& renderer() { return s_renderer; }
std::shared_ptr<spdlog::logger>& app() { return s_app; }

} // namespace fjell::log
