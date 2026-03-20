#pragma once

#ifdef FJELL_ENABLE_TRACY

#include <tracy/Tracy.hpp>

// CPU profiling
#define FJELL_PROFILE_FRAME       FrameMark
#define FJELL_PROFILE_SCOPE       ZoneScoped
#define FJELL_PROFILE_SCOPE_N(name) ZoneScopedN(name)

// GPU profiling macros (TracyVkContext, TracyVkDestroy, TracyVkCollect, TracyVkZone)
// are used directly in vulkan_context.cpp behind #ifdef FJELL_ENABLE_TRACY.
// TracyVulkan.hpp requires <vulkan/vulkan.h> before inclusion, so it is NOT
// included here — only in files that already have Vulkan headers.

#else

#define FJELL_PROFILE_FRAME         (void)0
#define FJELL_PROFILE_SCOPE         (void)0
#define FJELL_PROFILE_SCOPE_N(name) (void)0

#endif
