#pragma once

#ifdef FJELL_ENABLE_TRACY

#include <tracy/Tracy.hpp>

// CPU profiling
#define FJELL_PROFILE_FRAME       FrameMark
#define FJELL_PROFILE_SCOPE       ZoneScoped
#define FJELL_PROFILE_SCOPE_N(name) ZoneScopedN(name)

// Dynamic-name CPU zone — label is computed at runtime (per-pass
// names during DAG declare/submit, per-viewport IDs, etc). Costs a
// strlen() per frame; prefer FJELL_PROFILE_SCOPE_N for fixed names.
#define FJELL_PROFILE_SCOPE_DYNAMIC(name_cstr) \
    ZoneTransientN(___tracy_cpu_zone_transient, name_cstr, true)

// Dynamic-name CPU zone from a std::string_view, which need not be
// null-terminated. Same per-frame cost as FJELL_PROFILE_SCOPE_DYNAMIC
// without the strlen().
#define FJELL_PROFILE_SCOPE_VIEW(name_view)                                        \
    tracy::ScopedZone ___tracy_cpu_zone_view(                                      \
        TracyLine, TracyFile, strlen(TracyFile), TracyFunction,                    \
        strlen(TracyFunction), (name_view).data(), (name_view).size(), -1, true)

// GPU zone around a Vulkan record path. Requires a `FrameContext& ctx`
// in scope (for `ctx.tracy_ctx` and `ctx.cmd`) and that the translation
// unit has already included `<tracy/TracyVulkan.hpp>` — which in turn
// needs `<vulkan/vulkan.h>` first. Pass/record implementations that
// want a GPU zone should include TracyVulkan after the Vulkan headers
// and then use this macro; it compiles to nothing when Tracy is off.
#define FJELL_GPU_ZONE(ctx, name) \
    TracyVkZone(static_cast<tracy::VkCtx*>((ctx).tracy_ctx), (ctx).cmd, name)

// Dynamic-name variant for passes where the zone label is computed at
// runtime (bloom mip index, fog ping-pong, etc). Costs a strlen() per
// frame; prefer FJELL_GPU_ZONE for fixed names.
#define FJELL_GPU_ZONE_DYNAMIC(ctx, name_cstr) \
    TracyVkZoneTransient(static_cast<tracy::VkCtx*>((ctx).tracy_ctx), \
                         ___tracy_gpu_zone_transient, (ctx).cmd, name_cstr, true)

// GPU zones used directly via TracyVkContext / TracyVkDestroy /
// TracyVkCollect live in vulkan_context.cpp — see there.

#else

#define FJELL_PROFILE_FRAME         (void)0
#define FJELL_PROFILE_SCOPE         (void)0
#define FJELL_PROFILE_SCOPE_N(name) (void)0
#define FJELL_PROFILE_SCOPE_DYNAMIC(name) (void)0
#define FJELL_PROFILE_SCOPE_VIEW(name)    (void)(name)
#define FJELL_GPU_ZONE(ctx, name)          (void)0
#define FJELL_GPU_ZONE_DYNAMIC(ctx, name)  (void)0

#endif
