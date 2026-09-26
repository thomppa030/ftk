// Log implementation for the fjlint tool. The parsers report every problem
// through the core logger as "file:line:column: severity: message", the
// compiler convention editors parse, so the pattern prints the message and
// nothing else.

#include "core/log.hpp"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <mutex>

namespace {

int s_problems = 0;

/// Counts every warning and error the parsers and loaders emit, so the
/// tool's exit status covers all of them, not only the ones it tallies
/// itself.
class CountingSink : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        if (msg.level >= spdlog::level::warn) ++s_problems;
    }
    void flush_() override {}
};

} // namespace

int fjlint_logged_problems() {
    return s_problems;
}

namespace fjell::log {

static std::shared_ptr<spdlog::logger> s_core;
static std::shared_ptr<spdlog::logger> s_renderer;
static std::shared_ptr<spdlog::logger> s_app;

void init() {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sink->set_pattern("%v");
    auto counter = std::make_shared<CountingSink>();
    spdlog::sinks_init_list sinks{sink, counter};

    s_core = std::make_shared<spdlog::logger>("CORE", sinks);
    s_renderer = std::make_shared<spdlog::logger>("GFX", sinks);
    s_app = std::make_shared<spdlog::logger>("APP", sinks);

    s_core->set_level(spdlog::level::warn);
    s_renderer->set_level(spdlog::level::warn);
    s_app->set_level(spdlog::level::warn);
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
