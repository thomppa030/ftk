#pragma once

#include "core/result.hpp"
#include "gpu/access.hpp"
#include "gpu/binding.hpp"
#include "gpu/clear.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/render_encoder.hpp"
#include "gpu/shader.hpp"
#include "gpu/texture.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <source_location>
#include <span>
#include <string_view>
#include <type_traits>

namespace fjell::gpu {

class CommandList;
class Device;

/// A named span of a list's commands, open until it goes out of scope: a GPU
/// zone in the profiler and a label debuggers show. Zones nest.
class Zone {
public:
    ~Zone();

    Zone(const Zone&) = delete;
    Zone& operator=(const Zone&) = delete;
    Zone(Zone&&) = delete;
    Zone& operator=(Zone&&) = delete;

private:
    friend class CommandList;
    explicit Zone(CommandList& list) noexcept : list_(&list) {}

    CommandList* list_;
};

/// Records GPU work in order: a pass's commands, into the frame's command
/// buffer. A list starts each pass with nothing bound. Drawing happens in a
/// render scope, `render()`, while which the list records nothing itself.
/// A list belongs to one thread; lists on different threads record at once,
/// and `execute()` plays them into one.
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
    void bind(std::initializer_list<BindEntry> entries) {
        bind(std::span(entries.begin(), entries.size()));
    }
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
        const std::span all(std::ranges::data(values), std::ranges::size(values));
        return transient_bytes(std::as_bytes(all));
    }

    /// Runs the pipeline over `x` × `y` × `z` workgroups.
    void dispatch(uint32_t x, uint32_t y, uint32_t z);

    /// Runs the pipeline over the workgroups `args` holds (three `uint32_t`s),
    /// written by the GPU or the CPU earlier.
    void dispatch_indirect(BufferRange args);

    // Copies and clears. A texture is in `Access::copy_src` to be copied
    // from, `Access::copy_dst` to be copied into, `Access::clear` to be
    // cleared; a buffer needs nothing. A copy or clear that cannot be done is
    // reported once and not recorded.

    /// Copies `src` into `dst`, which must be as large; a range reaching to
    /// its buffer's end is the rest of the buffer.
    void copy(BufferRange src, BufferRange dst);

    /// Copies texels between two views of the same size and texel size, mip
    /// for mip and layer for layer.
    void copy(const TextureView& src, const TextureView& dst);

    /// Copies texels packed row after row, layer after layer, from `src` into
    /// one mip of a texture.
    void copy(BufferRange src, const TextureView& dst);

    /// Copies one mip of a texture into `dst`, packed row after row, layer
    /// after layer.
    void copy(const TextureView& src, BufferRange dst);

    /// Sets every texel of `view` to `value`.
    void clear(const TextureView& view, const Clear& value);

    /// Sets every 32-bit word of `range` to `value`: its offset and size are
    /// multiples of four.
    void fill(BufferRange range, uint32_t value);

    /// Fills each mip of a colour texture after the first from the one
    /// before, filtered, every layer. The texture is in `Access::copy_dst`
    /// before and after.
    void generate_mipmaps(Texture texture);

    /// Begins drawing into `targets` until the returned encoder goes out of
    /// scope. Targets that cannot be drawn to are reported, and the encoder
    /// records nothing.
    [[nodiscard]] RenderEncoder render(const RenderTargets& targets);

    // Barriers between a pass's own commands. Between passes the frame graph
    // orders what they declare.

    /// Orders what comes after on `view` as `after` behind what came before
    /// as `before`. Nothing before discards what the view holds.
    void barrier(const TextureView& view, AccessSet before, AccessSet after);
    void barrier(BufferRange range, AccessSet before, AccessSet after);

    /// Plays lists recorded on other threads, in order, as if what they hold
    /// were recorded here. Each holds whole passes: none of them, nor this
    /// list, is inside a render scope.
    void execute(std::span<CommandList* const> recorded_in_parallel);

    /// Opens a zone named `name` until the returned one goes out of scope;
    /// the profiler shows where it was opened. A zone may be opened inside a
    /// render scope too.
    ///
    /// @code
    /// auto zone = cmd.zone("Cloud Shadow");
    /// @endcode
    [[nodiscard]] Zone zone(std::string_view name,
                            std::source_location where = std::source_location::current());

    [[nodiscard]] Device& device() const noexcept { return *device_; }

    /// The backend's state, for the backend's own code.
    [[nodiscard]] Impl& impl() const noexcept { return *impl_; }

private:
    friend class Zone;

    void push_bytes(std::span<const std::byte> bytes);
    [[nodiscard]] BufferRange transient_bytes(std::span<const std::byte> bytes);
    void end_zone();

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
