#pragma once

#include "ftk/base/result.hpp"
#include "ftk/gpu/flags.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fjell::gpu {

/// A shader stage the GPU interface runs. Ray tracing stages are not among
/// them: rays are traced from compute with ray queries.
enum class ShaderStage : uint8_t {
    vertex,
    fragment,
    compute,
    /// Vulkan's task shader, Metal's object shader.
    task,
    mesh,
};

template <>
inline constexpr bool is_flag_enum<ShaderStage> = true;

/// A set of `ShaderStage`s.
using ShaderStages = Flags<ShaderStage>;

/// What a shader binds at a set and binding.
enum class BindingKind : uint8_t {
    uniform_buffer,
    storage_buffer,
    /// A texture and its sampler together (`sampler2D`).
    sampled_texture,
    /// A texture sampled with a sampler bound apart (`texture2D`).
    texture,
    sampler,
    /// Read or written as storage (`image2D`).
    storage_texture,
    acceleration_structure,
};

/// One binding a shader declares.
struct ShaderBinding {
    /// The variable's name: a block's instance name, or its type's name when
    /// the block has no instance name.
    std::string name{};
    /// A uniform or storage block's type name; empty for anything else.
    std::string block{};
    uint32_t set{0};
    uint32_t binding{0};
    BindingKind kind{BindingKind::uniform_buffer};
    /// Array length; 0 for an array sized at run time (`textures[]`).
    uint32_t count{1};
    /// Declared as an array. One of a single element is still an array: an
    /// unsized `textures[]` indexed only by constants compiles to as many
    /// elements as the highest index reaches, often one, and binds wherever
    /// the whole table would.
    bool array{false};
    /// The stages that declare it.
    ShaderStages stages{};

    bool operator==(const ShaderBinding&) const = default;
};

/// What a shader, or the shaders of one pipeline together, bind and take:
/// read from the SPIR-V, never written by hand.
struct ShaderLayout {
    ShaderStages stages{};
    /// Ordered by set, then binding.
    std::vector<ShaderBinding> bindings;
    /// Bytes of push data; 0 when no stage takes any.
    uint32_t push_size{0};
    ShaderStages push_stages{};
    /// Threads per workgroup of the compute, task and mesh stages, by stage;
    /// zeros for a stage the layout does not have.
    std::array<std::array<uint32_t, 3>, 5> workgroup_sizes{};
    /// What a mesh shader declares it outputs at most per workgroup.
    uint32_t mesh_max_vertices{0};
    uint32_t mesh_max_primitives{0};

    /// The binding named `name`, or null.
    [[nodiscard]] const ShaderBinding* find(std::string_view name) const;

    /// Threads per workgroup of `stage`.
    [[nodiscard]] const std::array<uint32_t, 3>& workgroup_size(ShaderStage stage) const {
        return workgroup_sizes[static_cast<size_t>(stage)];
    }
};

/// The SPIR-V in the file at `path`, word by word: a shader compiled at run
/// time, which `ShaderCode` then carries in memory.
/// @return the words, or why not: the file cannot be read, or holds no whole
///         number of words.
[[nodiscard]] Result<std::vector<uint32_t>> read_spirv(const std::string& path);

/// What one compiled shader binds and takes.
/// @return the layout, or why it cannot be read: not SPIR-V, no entry point,
///         or a stage the interface does not run.
[[nodiscard]] Result<ShaderLayout> reflect(std::span<const uint32_t> spirv);

/// The layout of a pipeline made of the stages of `a` and `b`. A set and
/// binding both declare must agree on its name, kind and count; push data
/// covers the larger of the two.
/// @return the joined layout, or which binding the two disagree on.
[[nodiscard]] Result<ShaderLayout> merge(const ShaderLayout& a, const ShaderLayout& b);

} // namespace fjell::gpu
