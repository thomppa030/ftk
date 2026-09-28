#pragma once

#include "gpu/access.hpp"

#include <vulkan/vulkan.h>

#include <optional>

// What each `gpu::Access` is to Vulkan: the layout an image is in for it, the
// pipeline stages and memory accesses a barrier names for it, and what an
// image must be made able to do. The frame graph's barriers and a command
// list's in-pass barriers are both derived from these.

namespace fjell::gpu::vulkan {

/// An access to an image.
struct ImageScope {
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkPipelineStageFlags2 stages{VK_PIPELINE_STAGE_2_NONE};
    VkAccessFlags2 access{VK_ACCESS_2_NONE};
};

/// An access to a buffer.
struct BufferScope {
    VkPipelineStageFlags2 stages{VK_PIPELINE_STAGE_2_NONE};
    VkAccessFlags2 access{VK_ACCESS_2_NONE};
};

/// What `access` is to an image; nothing (an undefined layout, no stages) for
/// an access that does not apply to textures.
/// @param depth whether the image holds depth, which is sampled in a
///        read-only depth layout rather than the colour one
[[nodiscard]] ImageScope image_scope(Access access, bool depth);

/// What `accesses` are to an image together: one layout, every stage and
/// access. An empty set is nothing: an undefined layout (the contents may be
/// discarded) and no stages.
/// @return the scope, or nothing when the accesses need different layouts
[[nodiscard]] std::optional<ImageScope> image_scope(AccessSet accesses, bool depth);

/// What `access` is to a buffer; no stages for an access that does not apply
/// to buffers.
[[nodiscard]] BufferScope buffer_scope(Access access);

/// What `accesses` are to a buffer together.
[[nodiscard]] BufferScope buffer_scope(AccessSet accesses);

/// What an image must be made able to do to be used as `access`.
[[nodiscard]] VkImageUsageFlags image_usage(Access access);

/// `stages` as a compute queue can wait on them: the stages only a graphics
/// queue has (attachments, fragment and vertex work, blits, resolves) become
/// all commands. Queue submissions order the two queues; within one, all
/// commands orders at least as much.
[[nodiscard]] VkPipelineStageFlags2 compute_queue_stages(VkPipelineStageFlags2 stages);

} // namespace fjell::gpu::vulkan
