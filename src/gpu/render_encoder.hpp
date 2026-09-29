#pragma once

#include "gpu/access.hpp"
#include "gpu/binding.hpp"
#include "gpu/buffer.hpp"
#include "gpu/clear.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/texture.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <vector>

namespace fjell::gpu {

class CommandList;
class Device;

/// What an attachment holds when rendering starts.
enum class Load : uint8_t {
    /// What it held before.
    load,
    /// Its clear value.
    clear,
    /// Nothing defined: everything drawn covers it.
    discard,
};

/// What an attachment keeps when rendering ends.
enum class Store : uint8_t {
    store,
    /// Nothing: only what it resolves into, or nothing at all, is read later.
    discard,
};

/// How a multisampled depth attachment resolves into a single-sample one.
enum class DepthResolve : uint8_t {
    sample_zero,
    min,
    max,
};

/// A colour texture drawn to. The texture is in `Access::color_attachment`,
/// and so is its resolve target.
struct ColorAttachment {
    TextureView view{};
    Load load{Load::load};
    Store store{Store::store};
    Clear clear{};
    /// The single-sample texture a multisampled view averages into when
    /// rendering ends; none while its texture is unset.
    TextureView resolve{};
};

/// The depth texture tested against. Depth formats with stencil are not
/// rendered to yet.
struct DepthAttachment {
    TextureView view{};
    /// How the pass uses it, which the pass declares too:
    /// `depth_attachment` tests and writes, `depth_attachment_read` only
    /// tests, `depth_read_sampled` tests while the shaders sample it.
    Access access{Access::depth_attachment};
    Load load{Load::load};
    /// What a written depth keeps; a depth only tested stores nothing,
    /// whatever this says.
    Store store{Store::store};
    Clear clear{Clear::depth_stencil(1.0f)};
    /// The single-sample texture a multisampled view resolves into, in
    /// `Access::depth_resolve`; none while its texture is unset.
    TextureView resolve{};
    DepthResolve resolve_mode{DepthResolve::sample_zero};
};

/// What `CommandList::render` draws to.
///
/// @code
/// auto pass = cmd.render({
///     .color = {{.view = output, .load = gpu::Load::clear}},
///     .depth = gpu::DepthAttachment{.view = depth, .access = gpu::Access::depth_attachment_read},
/// });
/// @endcode
struct RenderTargets {
    std::vector<ColorAttachment> color{};
    std::optional<DepthAttachment> depth{};
    /// The area drawn from the top left corner; 0 × 0 is the first
    /// attachment's size.
    uint32_t width{0};
    uint32_t height{0};
};

/// Where drawing lands in the targets, in pixels from the top left, and the
/// range of depth it writes.
struct Viewport {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
    float min_depth{0.0f};
    float max_depth{1.0f};
};

/// A rectangle of pixels from the top left.
struct Rect {
    int32_t x{0};
    int32_t y{0};
    uint32_t width{0};
    uint32_t height{0};
};

/// One draw's arguments as `RenderEncoder::draw_indexed_indirect` reads them
/// from a buffer, in this order on every backend.
struct DrawIndexedArgs {
    uint32_t index_count{0};
    uint32_t instance_count{0};
    uint32_t first_index{0};
    int32_t vertex_offset{0};
    uint32_t first_instance{0};
};
static_assert(sizeof(DrawIndexedArgs) == 20);

/// One mesh draw's task workgroup counts as
/// `RenderEncoder::draw_mesh_tasks_indirect` reads them from a buffer.
struct DrawMeshTasksArgs {
    uint32_t x{0};
    uint32_t y{1};
    uint32_t z{1};
};
static_assert(sizeof(DrawMeshTasksArgs) == 12);

/// The size of an index.
enum class IndexType : uint8_t {
    u16,
    u32,
};

/// Draws into the targets `CommandList::render` began; rendering ends when
/// the encoder goes out of scope. It starts with nothing bound, the viewport
/// and scissor over the whole area.
///
/// While it is open the list it came from records nothing else, and nothing
/// is ordered inside it: a pass that needs a barrier ends the scope and
/// begins another. Mistakes are reported as the list reports them, and a
/// draw that cannot run is not recorded.
class RenderEncoder {
public:
    ~RenderEncoder();

    RenderEncoder(const RenderEncoder&) = delete;
    RenderEncoder& operator=(const RenderEncoder&) = delete;
    RenderEncoder(RenderEncoder&&) = delete;
    RenderEncoder& operator=(RenderEncoder&&) = delete;

    /// The pipeline the next binds, pushes and draws are for. Its colour and
    /// depth formats and samples must be the targets'.
    void set_pipeline(GraphicsPipeline pipeline);

    /// As `CommandList::bind`, for the graphics pipeline.
    void bind(BindGroup group);
    void bind(std::initializer_list<BindEntry> entries) {
        bind(std::span(entries.begin(), entries.size()));
    }
    void bind(std::span<const BindEntry> entries);

    /// As `CommandList::push`, for the graphics pipeline.
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void push(const T& value) {
        static_assert(sizeof(T) <= 128, "Push data is at most 128 bytes on every GPU");
        push_bytes(std::as_bytes(std::span(&value, 1)));
    }

    /// As `CommandList::transient`: a copy of `value` in memory that lasts
    /// this frame, for a draw in this scope to bind.
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

    void set_viewport(const Viewport& viewport);
    void set_scissor(const Rect& scissor);

    /// The vertices a pipeline with a vertex layout reads.
    void set_vertex_buffer(BufferRange vertices);
    void set_index_buffer(BufferRange indices, IndexType type);

    void draw(uint32_t vertex_count, uint32_t instance_count = 1, uint32_t first_vertex = 0,
              uint32_t first_instance = 0);
    void draw_indexed(uint32_t index_count, uint32_t instance_count = 1, uint32_t first_index = 0,
                      int32_t vertex_offset = 0, uint32_t first_instance = 0);
    /// `count` indexed draws whose arguments `args` holds, `stride` bytes apart.
    void draw_indexed_indirect(BufferRange args, uint32_t count, uint32_t stride);

    /// Runs the mesh pipeline's task (or mesh) shader over `x` × `y` × `z`
    /// workgroups.
    void draw_mesh_tasks(uint32_t x, uint32_t y, uint32_t z);

    /// `draws` mesh draws whose workgroup counts `args` holds, `stride` bytes
    /// apart. `max_groups` is the most task groups any of them asks for: a
    /// GPU that cannot read a mesh draw from a buffer launches that many and
    /// the task shader stops past its draw's own count.
    void draw_mesh_tasks_indirect(BufferRange args, uint32_t draws, uint32_t stride,
                                  uint32_t max_groups);

    /// As `draw_mesh_tasks_indirect`, the number of draws read from `count`
    /// (one `uint32_t`), at most `max_draws`.
    void draw_mesh_tasks_indirect_count(BufferRange args, BufferRange count, uint32_t max_draws,
                                        uint32_t stride, uint32_t max_groups);

private:
    friend class CommandList;

    RenderEncoder(Device& device, CommandList& list, bool open) noexcept
        : device_(&device), list_(&list), open_(open) {}

    void push_bytes(std::span<const std::byte> bytes);
    [[nodiscard]] BufferRange transient_bytes(std::span<const std::byte> bytes);

    Device* device_;
    CommandList* list_;
    /// False when the targets were refused: nothing is recorded.
    bool open_;
};

} // namespace fjell::gpu
