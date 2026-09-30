#pragma once

#include "core/handle.hpp"
#include "core/result.hpp"
#include "gpu/binding.hpp"
#include "gpu/buffer.hpp"
#include "gpu/owned.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/resources/fjsl_parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace fjell {

namespace gpu {
class Device;
}

class Texture;

// Material data as stored in the GPU storage buffer (128 bytes).
// Indexed by material ID from push constants.
// Layout:
//   texture_indices[0-3]:   fixed PBR (base_color, normal, metallic_roughness, emissive)
//   texture_indices[4-15]:  custom shader textures. Twelve rather than four
//                           because a composed shader shares them with every
//                           shader_function inlined into it, and four is one
//                           texture each for a sky and three contributions.
//   base_params[0..3]:    base colour factor (vec4) — the file's factor times
//                         the shader's hint_albedo product; multiplies the
//                         base colour texture on every path
//   base_params[4]:       metallic_factor
//   base_params[5]:       roughness_factor
//   base_params[6]:       has_normal_map
//   base_params[7]:       uv_scale_x (default 1.0)
//   base_params[8]:       uv_scale_y (default 1.0)
//   base_params[9..11]:   emission factor — scales the emissive texture on
//                         every path; the emission itself when no texture
//                         is bound (see has_emissive_texture); zero when
//                         the material emits nothing
//   param_offset:         offset into custom params SSBO (in floats)
//   param_count:          number of custom floats
//   has_emissive_texture: 0/1 — whether texture_indices[3] is a texture the
//                         material bound, as opposed to the black default
//                         every unbound slot resolves to. The GI paths need
//                         it to tell "no texture" from "texture, black here".
struct GpuMaterial {
    uint32_t texture_indices[TOTAL_TEXTURE_SLOTS]{}; // 64 bytes — 0-3 PBR, 4-15 custom
    float base_params[12]{};            // 48 bytes — PBR + GI params
    uint32_t param_offset{0};           // 4 bytes  — offset into params SSBO (in floats)
    uint32_t param_count{0};            // 4 bytes  — number of custom floats
    uint32_t has_emissive_texture{0};   // 4 bytes  — see layout note above
    uint32_t padding{0};                // 4 bytes  — align to 128
};

// This struct is mirrored by hand in shaders/include/fjell_gpu_types.glsl, and
// every shader that reads a material reads it through that copy. A silent
// disagreement here is a whole class of bug that has cost us twice, so the
// layout is pinned: change one side and these stop the build until the other
// follows.
static_assert(sizeof(GpuMaterial) == 128, "GpuMaterial must match its GLSL mirror");
static_assert(offsetof(GpuMaterial, texture_indices) == 0);
static_assert(offsetof(GpuMaterial, base_params) == 64);
static_assert(offsetof(GpuMaterial, param_offset) == 112);
static_assert(offsetof(GpuMaterial, param_count) == 116);
static_assert(offsetof(GpuMaterial, has_emissive_texture) == 120);
static_assert(offsetof(GpuMaterial, padding) == 124);
static_assert(sizeof(GpuMaterial) % 16 == 0, "std430 wants a 16-byte multiple");

/// The table every material reads: textures by index, material rows and
/// their custom parameters, in one shared group the passes bind. Shaders read
/// textures as `textures[nonuniformEXT(index)]` and a row as
/// `materials[material_id]`.
///
/// Each frame slot has a group of its own, with its own material and
/// parameter buffers, so what a frame reads is never written while it runs:
/// changes are kept until `flush()` for the slot recording, which writes them
/// into that slot's group and buffers.
class BindlessTextures {
public:
    static constexpr uint32_t MAX_TEXTURES = 1024;
    static constexpr uint32_t MAX_MATERIALS = 1024;

    /// Makes the table's shared layout and a group for each frame slot.
    /// @return nothing, or what could not be made.
    [[nodiscard]] Result<> create(gpu::Device& gpu);
    void destroy();

    /// The texture's index, the same one each time it is added. Past
    /// `MAX_TEXTURES` the error is reported once and index 0 returned.
    uint32_t add(const Texture& texture);

    /// An index holding nothing until `update_entry()` gives it a texture;
    /// shaders must not read it before.
    [[nodiscard]] uint32_t reserve_index();

    /// Points an index at another texture and sampler, for the frames that
    /// flush after it (a render target that was resized, a font atlas baked
    /// again). No texture empties it, before what it held is destroyed.
    void update_entry(uint32_t index, gpu::Texture texture, gpu::Sampler sampler);

    /// Register a material and get its index for push constants.
    uint32_t add_material(const GpuMaterial& mat);

    /// Update an existing material row.
    void update_material(uint32_t index, const GpuMaterial& mat);

    /// Allocate a contiguous range of floats in the custom params buffer.
    /// Returns the offset (in floats) into it.
    uint32_t allocate_params(uint32_t count);

    /// Write custom param values at a given offset.
    void write_params(uint32_t offset, const float* data, uint32_t count);

    /// Writes what changed since `slot` last flushed into its group and
    /// buffers. Called before a view's passes bind the table; later calls
    /// in the same frame write only what changed since.
    void flush(uint32_t slot);

    /// The layout pipelines name to bind the table.
    [[nodiscard]] gpu::SharedLayout layout() const { return layout_; }
    /// The group the frames of `slot` bind.
    [[nodiscard]] gpu::BindGroup group(uint32_t slot) const { return slots_[slot].group; }

    [[nodiscard]] const GpuMaterial& material_at(uint32_t index) const { return materials_[index]; }
    [[nodiscard]] uint32_t material_count() const { return static_cast<uint32_t>(materials_.size()); }

    // Read a custom param value by absolute offset (for UI readback)
    [[nodiscard]] float read_param(uint32_t offset) const {
        return offset < params_.size() ? params_[offset] : 0.0f;
    }

private:
    static constexpr uint32_t INITIAL_PARAMS_CAPACITY = 4096; // floats

    struct Entry {
        gpu::Texture texture{};
        gpu::Sampler sampler{};
    };

    /// One frame slot's group, its buffers, and what it has yet to take.
    struct Slot {
        gpu::Owned<gpu::BindGroup> group;
        gpu::Owned<gpu::Buffer> materials;
        gpu::Owned<gpu::Buffer> params;
        uint64_t params_capacity{0}; // floats
        std::vector<uint32_t> pending_textures;
        bool materials_pending{false};
        bool params_pending{false};
    };

    /// Marks a texture index for every slot to write at its next flush.
    void mark_texture(uint32_t index);
    /// A table index for a new entry, or none when the table is full.
    [[nodiscard]] std::optional<uint32_t> next_index();

    gpu::Device* gpu_{nullptr};
    gpu::SharedLayout layout_{};
    std::array<Slot, MAX_FRAMES_IN_FLIGHT> slots_;

    std::vector<Entry> entries_;
    /// For each entry, the slots that have yet to write it, one bit each.
    std::vector<uint8_t> entry_pending_;
    std::unordered_map<gpu::Texture, uint32_t, HandleHash> texture_to_index_;
    bool texture_overflow_reported_{false};

    std::vector<GpuMaterial> materials_;
    /// Set once the rows outgrow the buffers, so the error is said once
    /// rather than every frame.
    bool material_overflow_reported_{false};

    std::vector<float> params_;
};

} // namespace fjell
