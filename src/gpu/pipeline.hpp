#pragma once

#include "core/handle.hpp"
#include "gpu/compare.hpp"
#include "gpu/flags.hpp"
#include "gpu/format.hpp"

#include <concepts>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace fjell::gpu {

class Device;
struct ComputePipelineTag;
struct GraphicsPipelineTag;
struct SharedLayoutTag;

/// A compute pipeline, by handle. Made by `Device::create`, held by
/// `Owned<ComputePipeline>`; `Device::recreate` rebuilds it in place.
using ComputePipeline = Handle<ComputePipelineTag>;

/// A graphics pipeline (vertex or mesh), by handle. Made by `Device::create`,
/// held by `Owned<GraphicsPipeline>`; `Device::recreate` rebuilds it in place.
using GraphicsPipeline = Handle<GraphicsPipelineTag>;

/// Hands a pipeline back to its device. `Owned<>` calls them.
void release(Device& device, ComputePipeline pipeline);
void release(Device& device, GraphicsPipeline pipeline);

/// A set layout the engine shares between the pipelines that name it (the
/// globals, the bindless table, draw data, meshlets), by handle. It lasts as
/// long as the device.
using SharedLayout = Handle<SharedLayoutTag>;

/// A compute or a graphics pipeline, where either is taken.
struct PipelineRef {
    ComputePipeline compute{};
    GraphicsPipeline graphics{};

    constexpr PipelineRef(ComputePipeline pipeline) : compute(pipeline) {}
    constexpr PipelineRef(GraphicsPipeline pipeline) : graphics(pipeline) {}

    /// From anything that holds one (`Owned<ComputePipeline>`).
    template <typename T>
        requires(std::convertible_to<const T&, ComputePipeline> &&
                 !std::same_as<T, ComputePipeline>)
    constexpr PipelineRef(const T& held) : compute(static_cast<ComputePipeline>(held)) {}
    template <typename T>
        requires(std::convertible_to<const T&, GraphicsPipeline> &&
                 !std::same_as<T, GraphicsPipeline>)
    constexpr PipelineRef(const T& held) : graphics(static_cast<GraphicsPipeline>(held)) {}
};

/// A compiled shader: the path of one the build compiled (`"shaders/grid.vert"`
/// names `shaders/grid.vert.spv` beside the program, found through the device's
/// locator), or SPIR-V in memory (what FJSL compiles to), which must outlive the
/// create call only.
struct ShaderCode {
    std::string_view path{};
    std::span<const uint32_t> spirv{};

    constexpr ShaderCode() = default;
    constexpr ShaderCode(const char* shader_path) : path(shader_path) {}
    constexpr ShaderCode(std::string_view shader_path) : path(shader_path) {}
    constexpr ShaderCode(std::span<const uint32_t> code) : spirv(code) {}

    /// Whether no shader is given.
    [[nodiscard]] constexpr bool empty() const { return path.empty() && spirv.empty(); }
};

/// What `Device::create` makes a compute pipeline from. Its layout comes from
/// the shader.
///
/// @code
/// auto pipeline = device.create(gpu::ComputePipelineDesc{
///     .shader = "shaders/cloud_shadow.comp",
///     .name = "cloud_shadow",
/// });
/// @endcode
struct ComputePipelineDesc {
    ShaderCode shader{};
    /// The shared layouts the pipeline binds; each takes the set the shader
    /// declares it at (see `place_shared`). Every other set is the pipeline's
    /// own.
    std::vector<SharedLayout> shared{};
    /// Shown by debuggers and in error messages; not kept.
    std::string_view name{};
};

/// A factor a blend multiplies the source or the destination by.
enum class BlendFactor : uint8_t {
    zero,
    one,
    src_alpha,
    one_minus_src_alpha,
    dst_color,
    dst_alpha,
};

/// How a colour target combines what is drawn with what it holds:
/// `src * src_factor + dst * dst_factor`, for colour and alpha apart.
/// `Blend::none` writes what is drawn.
struct Blend {
    bool enabled{false};
    BlendFactor src_color{BlendFactor::one};
    BlendFactor dst_color{BlendFactor::zero};
    BlendFactor src_alpha{BlendFactor::one};
    BlendFactor dst_alpha{BlendFactor::zero};

    /// No blending.
    static const Blend none;
    /// Colour over what is there by the source's alpha; coverage accumulates.
    static const Blend alpha;
    /// Colour and alpha added.
    static const Blend additive;
    /// Colour and alpha multiplied into what is there.
    static const Blend multiply;

    bool operator==(const Blend&) const = default;
};

inline constexpr Blend Blend::none{};
inline constexpr Blend Blend::alpha{true, BlendFactor::src_alpha, BlendFactor::one_minus_src_alpha,
                                    BlendFactor::one, BlendFactor::one_minus_src_alpha};
inline constexpr Blend Blend::additive{true, BlendFactor::one, BlendFactor::one, BlendFactor::one,
                                       BlendFactor::one};
inline constexpr Blend Blend::multiply{true, BlendFactor::dst_color, BlendFactor::zero,
                                       BlendFactor::dst_alpha, BlendFactor::zero};

/// A channel of a colour target.
enum class Channel : uint8_t { r, g, b, a };

template <>
inline constexpr bool is_flag_enum<Channel> = true;

/// The channels a pipeline writes to a colour target.
using ChannelMask = Flags<Channel>;

/// Every channel.
inline constexpr ChannelMask ALL_CHANNELS = Channel::r | Channel::g | Channel::b | Channel::a;

/// A colour attachment a graphics pipeline draws to.
struct ColorTarget {
    Format format{Format::undefined};
    Blend blend{};
    /// The channels written; an empty mask draws nothing to this target.
    ChannelMask write{ALL_CHANNELS};
};

/// What a vertex buffer's elements are made of.
struct VertexAttribute {
    uint32_t location{0};
    VertexFormat format{VertexFormat::float3};
    /// Bytes from the start of the element.
    uint32_t offset{0};
};

/// The one vertex buffer a vertex pipeline reads, if any.
struct VertexLayout {
    /// Bytes per element; 0 when the pipeline reads no vertex buffer.
    uint32_t stride{0};
    std::vector<VertexAttribute> attributes{};
};

enum class Topology : uint8_t {
    triangles,
    triangle_strip,
    lines,
};

enum class Cull : uint8_t {
    none,
    back,
    front,
};

enum class FrontFace : uint8_t {
    counter_clockwise,
    clockwise,
};

enum class Fill : uint8_t {
    solid,
    /// Edges only.
    lines,
};

/// How primitives become fragments.
struct Raster {
    Cull cull{Cull::none};
    FrontFace front_face{FrontFace::counter_clockwise};
    Fill fill{Fill::solid};
    /// Clamps depth to the range instead of clipping (shadow casters behind
    /// the near plane).
    bool depth_clamp{false};
    /// Depth bias: constant, per unit of slope, and the most it may add; all
    /// zero is none.
    float depth_bias{0.0f};
    float depth_bias_slope{0.0f};
    float depth_bias_clamp{0.0f};
};

/// The depth test and write.
struct Depth {
    bool test{false};
    bool write{false};
    Compare compare{Compare::less};
};

/// What `Device::create` makes a graphics pipeline from. Its layout comes from
/// the shaders. Only shaders and one colour target given, it is the fullscreen
/// pipeline: no vertex buffer, triangles, no depth, no culling.
///
/// @code
/// auto grid = device.create(gpu::GraphicsPipelineDesc{
///     .vertex = "shaders/grid.vert",
///     .fragment = "shaders/grid.frag",
///     .color = {{PostPass::OUTPUT_FORMAT, gpu::Blend::alpha}},
///     .name = "grid",
/// });
/// @endcode
struct GraphicsPipelineDesc {
    /// A vertex pipeline gives `vertex`; a mesh pipeline gives `mesh`, and
    /// `task` when it has one.
    ShaderCode vertex{};
    ShaderCode task{};
    ShaderCode mesh{};
    /// Left out, nothing is shaded (depth-only drawing).
    ShaderCode fragment{};
    VertexLayout vertex_layout{};
    Topology topology{Topology::triangles};
    Raster raster{};
    Depth depth{};
    std::vector<ColorTarget> color{};
    Format depth_format{Format::undefined};
    Samples samples{Samples::x1};
    /// The shared layouts the pipeline binds; each takes the set its shaders
    /// declare it at (see `place_shared`). Every other set is the pipeline's
    /// own.
    std::vector<SharedLayout> shared{};
    /// Shown by debuggers and in error messages; not kept.
    std::string_view name{};
};

} // namespace fjell::gpu
