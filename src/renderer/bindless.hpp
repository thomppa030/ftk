#pragma once

#include "core/result.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/deferred_deleter.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fjell {

class Texture;

// Material data as stored in the GPU storage buffer (112 bytes).
// Indexed by material ID from push constants.
// Layout:
//   texture_indices[0-3]:   fixed PBR (base_color, normal, metallic_roughness, emissive)
//   texture_indices[4-7]:   custom shader textures
//   base_params[0..3]:    base_color_factor (vec4)
//   base_params[4]:       metallic_factor
//   base_params[5]:       roughness_factor
//   base_params[6]:       has_normal_map
//   base_params[7]:       uv_scale_x (default 1.0)
//   base_params[8]:       uv_scale_y (default 1.0)
//   base_params[9..11]:   emission factor — scales the emissive texture on
//                         every path; the emission itself when no texture
//                         is bound (see has_emissive_texture); zero when
//                         the material emits nothing
//   base_params[12..14]:  gi_albedo_override (RT/SDF GI diffuse bounce
//                         color; only valid when has_gi_albedo_override
//                         != 0 — otherwise fall back to
//                         base_color_factor * base color texture)
//   param_offset:         offset into custom params SSBO (in floats)
//   param_count:          number of custom floats
//   has_gi_albedo_override: 0/1, see base_params[12..14]
//   has_emissive_texture: 0/1 — whether texture_indices[3] is a texture the
//                         material bound, as opposed to the black default
//                         every unbound slot resolves to. The GI paths need
//                         it to tell "no texture" from "texture, black here".
struct GpuMaterial {
    uint32_t texture_indices[8]{};      // 32 bytes — slots 0-3: PBR, 4-7: custom
    float base_params[15]{};            // 60 bytes — PBR + GI params
    uint32_t param_offset{0};           // 4 bytes  — offset into params SSBO (in floats)
    uint32_t param_count{0};            // 4 bytes  — number of custom floats
    uint32_t has_gi_albedo_override{0}; // 4 bytes  — see base_params[12..14]
    uint32_t has_emissive_texture{0};   // 4 bytes  — see layout note above
    uint32_t padding{0};                // 4 bytes  — align to 112
};

// Manages a single large descriptor set containing all textures and materials
// in the engine. Textures get a global index; materials are stored in an SSBO.
// Shaders access textures via textures[nonuniformEXT(index)] and materials
// via materials[push.material_id].
class BindlessTextures {
public:
    static constexpr uint32_t MAX_TEXTURES = 1024;
    static constexpr uint32_t MAX_MATERIALS = 1024;

    [[nodiscard]] Result<> create(VkDevice device, VmaAllocator allocator,
                                  DeferredDeleter& deleter);
    void destroy();

    // Register a texture and get its bindless index.
    // Idempotent: re-adding the same texture returns the existing index.
    uint32_t add(const Texture& texture);

    // Reserve a bindless index without writing a descriptor.
    // The slot is left unwritten (PARTIALLY_BOUND covers this).
    // Use update_entry() to associate a view later.
    [[nodiscard]] uint32_t reserve_index();

    // Replace an existing entry's view and sampler (e.g. after render target resize).
    // Marks the slot dirty for the next update_descriptors() call.
    void update_entry(uint32_t index, VkImageView view, VkSampler sampler);

    // Register a material and get its index for push constants.
    uint32_t add_material(const GpuMaterial& mat);

    // Update an existing material in the SSBO.
    void update_material(uint32_t index, const GpuMaterial& mat);

    // Allocate a contiguous range of floats in the custom params SSBO.
    // Returns the offset (in floats) into the params buffer.
    uint32_t allocate_params(uint32_t count);

    // Write custom param values into the params SSBO at a given offset.
    void write_params(uint32_t offset, const float* data, uint32_t count);

    // Flush pending texture descriptor writes.
    void update_descriptors();

    // Flush pending material data to the SSBO.
    void update_material_buffer();

    [[nodiscard]] VkDescriptorSetLayout layout() const { return layout_; }
    [[nodiscard]] VkDescriptorSet descriptor_set() const { return set_; }
    [[nodiscard]] const GpuMaterial& material_at(uint32_t index) const { return materials_[index]; }
    [[nodiscard]] uint32_t material_count() const { return static_cast<uint32_t>(materials_.size()); }

    // Read a custom param value by absolute offset (for UI readback)
    [[nodiscard]] float read_param(uint32_t offset) const {
        return offset < params_.size() ? params_[offset] : 0.0f;
    }

private:
    static constexpr uint32_t INITIAL_PARAMS_CAPACITY = 4096; // floats

    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    DeferredDeleter* deleter_{nullptr};

    VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSet set_{VK_NULL_HANDLE};
    VkSampler default_sampler_{VK_NULL_HANDLE};

    // Texture entries
    struct Entry {
        VkImageView view;
        VkSampler sampler;
    };
    std::vector<Entry> entries_;
    std::unordered_map<VkImageView, uint32_t> view_to_index_;
    uint32_t tex_dirty_from_{0};

    // Material SSBO (binding 1)
    std::vector<GpuMaterial> materials_;
    std::unique_ptr<Buffer> material_ssbo_;
    bool materials_dirty_{false};

    // Custom params SSBO (binding 2)
    std::vector<float> params_;
    std::unique_ptr<Buffer> params_ssbo_;
    bool params_dirty_{false};
};

} // namespace fjell
