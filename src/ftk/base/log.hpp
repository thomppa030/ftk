#pragma once

#include <spdlog/spdlog.h>

#include <memory>
#include <optional>
#include <string>

namespace ftk::log {

/// How init() sets the loggers up.
struct Options {
    /// The lowest level shown. Unset: info, unless the FTK_LOG_LEVEL
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

} // namespace ftk::log

// Core engine logging
#define FTK_CORE_TRACE(...)    ::ftk::log::core()->trace(__VA_ARGS__)
#define FTK_CORE_DEBUG(...)    ::ftk::log::core()->debug(__VA_ARGS__)
#define FTK_CORE_INFO(...)     ::ftk::log::core()->info(__VA_ARGS__)
#define FTK_CORE_WARN(...)     ::ftk::log::core()->warn(__VA_ARGS__)
#define FTK_CORE_ERROR(...)    ::ftk::log::core()->error(__VA_ARGS__)
#define FTK_CORE_CRITICAL(...) ::ftk::log::core()->critical(__VA_ARGS__)

// Renderer logging
#define FTK_GFX_TRACE(...)    ::ftk::log::renderer()->trace(__VA_ARGS__)
#define FTK_GFX_DEBUG(...)    ::ftk::log::renderer()->debug(__VA_ARGS__)
#define FTK_GFX_INFO(...)     ::ftk::log::renderer()->info(__VA_ARGS__)
#define FTK_GFX_WARN(...)     ::ftk::log::renderer()->warn(__VA_ARGS__)
#define FTK_GFX_ERROR(...)    ::ftk::log::renderer()->error(__VA_ARGS__)
#define FTK_GFX_CRITICAL(...) ::ftk::log::renderer()->critical(__VA_ARGS__)

// Application / game-side logging
#define FTK_APP_TRACE(...)    ::ftk::log::app()->trace(__VA_ARGS__)
#define FTK_APP_DEBUG(...)    ::ftk::log::app()->debug(__VA_ARGS__)
#define FTK_APP_INFO(...)     ::ftk::log::app()->info(__VA_ARGS__)
#define FTK_APP_WARN(...)     ::ftk::log::app()->warn(__VA_ARGS__)
#define FTK_APP_ERROR(...)    ::ftk::log::app()->error(__VA_ARGS__)
#define FTK_APP_CRITICAL(...) ::ftk::log::app()->critical(__VA_ARGS__)
