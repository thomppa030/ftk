#pragma once

#include "gpu/command_list.hpp"
#include "gpu/vulkan/device_impl.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell::gpu {

/// The Vulkan backend's recording state: the command buffer a list records
/// into and what has been bound since its pipeline was set.
struct CommandList::Impl {
    VkCommandBuffer cb{VK_NULL_HANDLE};
    /// Null until a pipeline is set.
    const Device::Impl::PipelineRecord* pipeline{nullptr};
    /// The sets bound since, one bit each.
    uint32_t bound_sets{0};
    /// Set by a bind or push that was refused: the list records no dispatch
    /// until the next pipeline.
    bool refused{false};
};

} // namespace fjell::gpu
