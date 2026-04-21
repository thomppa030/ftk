#pragma once

// Tiny helper: include this in any render pass .cpp that wants a
// `FJELL_GPU_ZONE(ctx, "name")` on its record() path. Wraps the Vulkan
// + Tracy header pair so callers don't have to repeat the #ifdef block.

#ifdef FJELL_ENABLE_TRACY
#include <vulkan/vulkan.h>
#include <tracy/TracyVulkan.hpp>
#endif

#include "core/profiler.hpp"
