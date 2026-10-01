#pragma once

#include "ftk/gpu/access.hpp"

#include <vulkan/vulkan.h>

#include <optional>
#include <string>

// What each `gpu::Access` is to Vulkan: the layout an image is in for it, and
// the pipeline stages and memory accesses a barrier names for it. The frame
// graph's barriers and a command list's in-pass barriers are both derived
// from these.

namespace ftk::gpu::vulkan {

/// An access to an image.
struct ImageScope {
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkPipelineStageFlags2 stages{VK_PIPELINE_STAGE_2_NONE};
    VkAccessFlags2 access{VK_ACCESS_2_NONE};
};

/// The access bits that write memory, as opposed to reading it: what a
/// barrier makes available. Every access that writes (`access_is_write`)
/// has one of them.
inline constexpr VkAccessFlags2 WRITE_ACCESS_BITS =
    VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
    VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT |
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;

/// An access to a buffer or an acceleration structure.
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

/// What `accesses` are to an image when one pass uses it all these ways at
/// once: every stage and access, in one layout that serves them all. Where
/// the accesses' own layouts differ, an attachment layout wins (a read-only
/// depth one serves sampling too), and anything else shares GENERAL. An
/// empty set is an undefined layout and no stages.
[[nodiscard]] ImageScope merged_image_scope(AccessSet accesses, bool depth);

/// What `access` is to a buffer, or to an acceleration structure, whose
/// memory is one; no stages for an access that applies to neither.
[[nodiscard]] BufferScope buffer_scope(Access access);

/// What `accesses` are to a buffer together.
[[nodiscard]] BufferScope buffer_scope(AccessSet accesses);

/// A layout by its short name ("SHADER_RO"), or its number for one without.
[[nodiscard]] std::string layout_name(VkImageLayout layout);

/// `stages` as a compute queue can wait on them: the stages only a graphics
/// queue has (attachments, fragment and vertex work, blits, resolves) become
/// all commands. Queue submissions order the two queues; within one, all
/// commands orders at least as much.
[[nodiscard]] VkPipelineStageFlags2 compute_queue_stages(VkPipelineStageFlags2 stages);

} // namespace ftk::gpu::vulkan
