#pragma once

#include "ftk/gpu/command_list.hpp"
#include "ftk/gpu/queue.hpp"
#include "ftk/gpu/vulkan/device_impl.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace fjell::gpu {

/// The Vulkan backend's recording state: the command buffer a list records
/// into and what has been bound since its pipeline was set. A render encoder
/// records through its list's state.
struct CommandList::Impl {
    VkCommandBuffer cb{VK_NULL_HANDLE};
    /// The queue `cb` is submitted to, which decides the stages a barrier
    /// may name.
    Queue queue{Queue::graphics};
    /// Null until a pipeline is set.
    const Device::Impl::PipelineRecord* pipeline{nullptr};
    /// The sets bound since, one bit each.
    uint32_t bound_sets{0};
    /// Set by a bind or push that was refused: the list records no dispatch
    /// or draw until the next pipeline.
    bool refused{false};

    /// While a render scope is open: the list records nothing itself.
    bool rendering{false};
    /// A list recorded on another thread (`Frame::parallel_commands`), still
    /// recording until it is played.
    bool parallel_open{false};
    /// What the open scope draws to, which a graphics pipeline must match.
    std::array<Format, MAX_COLOR_TARGETS> target_colors{};
    uint32_t target_color_count{0};
    Format target_depth{Format::undefined};
    Samples target_samples{Samples::x1};
    bool vertex_buffer_set{false};
    bool index_buffer_set{false};

    /// Zones open on the list; the profiler times the first `MAX_ZONE_DEPTH`.
    static constexpr uint32_t MAX_ZONE_DEPTH = 8;
    uint32_t zone_depth{0};
#ifdef FJELL_ENABLE_TRACY
    std::array<std::optional<tracy::VkCtxScope>, MAX_ZONE_DEPTH> zones{};
#endif
};

namespace vulkan {

/// Reports why the list refuses what it was asked, naming its pipeline; it
/// records no dispatch or draw until the next pipeline.
void refuse(Device& device, CommandList::Impl& list, const std::string& why);

/// Whether the list may record `what` itself: not while a render scope is
/// open, which is reported.
[[nodiscard]] bool outside_render(Device& device, CommandList::Impl& list, const char* what);

/// Binds `pipeline` and forgets what was bound for the one before; null (a
/// pipeline that no longer exists) is refused.
void use_pipeline(Device& device, CommandList::Impl& list,
                  const Device::Impl::PipelineRecord* pipeline);

/// A bind group, or resources by name, at the set they fill.
void bind_group(Device& device, CommandList::Impl& list, BindGroup group);
void bind_entries(Device& device, CommandList::Impl& list, std::span<const BindEntry> entries);

/// The pipeline's push data.
void push(Device& device, CommandList::Impl& list, std::span<const std::byte> bytes);

/// Whether the pipeline may run `what`: set, nothing refused, every set its
/// shaders declare bound.
[[nodiscard]] bool ready(Device& device, CommandList::Impl& list, const char* what);

} // namespace vulkan

} // namespace fjell::gpu
