#pragma once

#include "core/result.hpp"
#include "gpu/binding.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/shader.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <span>
#include <type_traits>

namespace fjell::gpu {

class Device;

/// Records GPU work in order: a pass's commands, into the frame's command
/// buffer. A list starts each pass with nothing bound.
///
/// What is bound is checked against what the pipeline's shaders declare. A
/// wrong binding or push is reported once, naming the pipeline and the
/// binding, and the list records no dispatch until the next `set_pipeline`,
/// so the GPU never runs a shader with something missing.
///
/// @code
/// auto& cmd = ctx.commands();
/// cmd.set_pipeline(pipeline_);
/// cmd.bind({{"shadow_out", gpu::storage(map_)}});
/// cmd.push(CloudShadowPush{.time = time_});
/// cmd.dispatch(groups, groups, 1);
/// @endcode
class CommandList {
public:
    /// The backend's recording state, which the backend defines.
    struct Impl;

    CommandList(Device& device, Impl& impl) noexcept : device_(&device), impl_(&impl) {}

    CommandList(const CommandList&) = delete;
    CommandList& operator=(const CommandList&) = delete;

    /// The pipeline the next binds, pushes and dispatches are for. Everything
    /// bound for the pipeline before is forgotten.
    void set_pipeline(ComputePipeline pipeline);

    /// A persistent group, at the set it was made for, or a shared group
    /// (globals, bindless, draw data, meshlets) at the set the pipeline's
    /// shaders declare it at.
    void bind(BindGroup group);

    /// Resources by the names the shaders give them, filling one of the
    /// pipeline's own sets for this frame. Two binds of the same resources
    /// in one frame share one set.
    void bind(std::initializer_list<BindEntry> entries) { bind(std::span(entries.begin(), entries.size())); }
    void bind(std::span<const BindEntry> entries);

    /// The pipeline's push data. The shaders read the first `push_size`
    /// bytes of it; a value shorter than that is refused.
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void push(const T& value) {
        static_assert(sizeof(T) <= 128, "Push data is at most 128 bytes on every GPU");
        push_bytes(std::as_bytes(std::span(&value, 1)));
    }

    /// A copy of `value` in memory that lasts this frame, to bind as a
    /// uniform or storage buffer or read as indirect arguments. Empty when
    /// no memory could be had, which is reported and refused where bound.
    template <typename T>
        requires(std::is_trivially_copyable_v<T> && !std::ranges::range<T>)
    [[nodiscard]] BufferRange transient(const T& value) {
        return transient_bytes(std::as_bytes(std::span(&value, 1)));
    }

    /// A copy of `values`, one after another, in memory that lasts this frame.
    template <std::ranges::contiguous_range R>
        requires std::is_trivially_copyable_v<std::ranges::range_value_t<R>>
    [[nodiscard]] BufferRange transient(const R& values) {
        return transient_bytes(std::as_bytes(std::span(std::ranges::data(values), std::ranges::size(values))));
    }

    /// Runs the pipeline over `x` × `y` × `z` workgroups.
    void dispatch(uint32_t x, uint32_t y, uint32_t z);

    /// Runs the pipeline over the workgroups `args` holds (three `uint32_t`s),
    /// written by the GPU or the CPU earlier.
    void dispatch_indirect(BufferRange args);

    [[nodiscard]] Device& device() const noexcept { return *device_; }

    /// The backend's state, for the backend's own code.
    [[nodiscard]] Impl& impl() const noexcept { return *impl_; }

private:
    void push_bytes(std::span<const std::byte> bytes);
    [[nodiscard]] BufferRange transient_bytes(std::span<const std::byte> bytes);

    Device* device_;
    Impl* impl_;
};

/// How many bytes of `given` a pipeline with `layout` reads as push data: its
/// push size, which `given` must cover.
/// @return the size, or why the push is wrong (the pipeline takes none, or
///         reads more than is given).
[[nodiscard]] Result<uint32_t> push_size(const ShaderLayout& layout, size_t given);

/// The sets `layout`'s shaders declare, one bit each (bit n for set n).
[[nodiscard]] uint32_t declared_sets(const ShaderLayout& layout);

} // namespace fjell::gpu
