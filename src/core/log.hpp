#pragma once

#include <spdlog/spdlog.h>

#include <memory>

namespace fjell::log {

void init();
void shutdown();

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
