#pragma once

#include <spdlog/spdlog.h>

#include <memory>
#include <optional>
#include <string>

namespace fjell::log {

/// How init() sets the loggers up.
struct Options {
    /// The lowest level shown. Unset: info, unless the FJELL_LOG_LEVEL
    /// environment variable names another (trace, debug, info, warn, error).
    std::optional<spdlog::level::level_enum> level{};
    /// How the terminal shows a line, in spdlog's pattern syntax.
    std::string pattern{"%^[%T.%e] [%n] [%l]%$ %v"};
};

/// Creates the CORE/GFX/APP loggers, writing to the terminal. Called again,
/// it starts over: new loggers, without the sinks added to the old ones.
void init(const Options& options = {});
void shutdown();

/// Sends every logger's messages to `sink` as well as the terminal, from the
/// next message on: an editor's console panel, a file. Call after init() and
/// before any other thread logs; shutdown() lets go of it.
void add_sink(spdlog::sink_ptr sink);

[[nodiscard]] std::shared_ptr<spdlog::logger>& core();
[[nodiscard]] std::shared_ptr<spdlog::logger>& renderer();
[[nodiscard]] std::shared_ptr<spdlog::logger>& app();

} // namespace fjell::log

// Core engine logging
#define FJELL_CORE_TRACE(...)    ::fjell::log::core()->trace(__VA_ARGS__)
#define FJELL_CORE_DEBUG(...)    ::fjell::log::core()->debug(__VA_ARGS__)
#define FJELL_CORE_INFO(...)     ::fjell::log::core()->info(__VA_ARGS__)
#define FJELL_CORE_WARN(...)     ::fjell::log::core()->warn(__VA_ARGS__)
#define FJELL_CORE_ERROR(...)    ::fjell::log::core()->error(__VA_ARGS__)
#define FJELL_CORE_CRITICAL(...) ::fjell::log::core()->critical(__VA_ARGS__)

// Renderer logging
#define FJELL_GFX_TRACE(...)    ::fjell::log::renderer()->trace(__VA_ARGS__)
#define FJELL_GFX_DEBUG(...)    ::fjell::log::renderer()->debug(__VA_ARGS__)
#define FJELL_GFX_INFO(...)     ::fjell::log::renderer()->info(__VA_ARGS__)
#define FJELL_GFX_WARN(...)     ::fjell::log::renderer()->warn(__VA_ARGS__)
#define FJELL_GFX_ERROR(...)    ::fjell::log::renderer()->error(__VA_ARGS__)
#define FJELL_GFX_CRITICAL(...) ::fjell::log::renderer()->critical(__VA_ARGS__)

// Application / game-side logging
#define FJELL_APP_TRACE(...)    ::fjell::log::app()->trace(__VA_ARGS__)
#define FJELL_APP_DEBUG(...)    ::fjell::log::app()->debug(__VA_ARGS__)
#define FJELL_APP_INFO(...)     ::fjell::log::app()->info(__VA_ARGS__)
#define FJELL_APP_WARN(...)     ::fjell::log::app()->warn(__VA_ARGS__)
#define FJELL_APP_ERROR(...)    ::fjell::log::app()->error(__VA_ARGS__)
#define FJELL_APP_CRITICAL(...) ::fjell::log::app()->critical(__VA_ARGS__)
