#pragma once

#include "renderer/gpu/buffer.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fjell {

class Device;
class Model;
struct PassResources;

// Per-instance SDF metadata for the compositing shader (matches GLSL SdfInstance)
struct GpuSdfInstance {
    glm::mat4 world_to_sdf;    // inverse model matrix scaled to SDF grid space
    glm::vec3 sdf_min;         // local-space SDF volume min
    float sdf_scale;           // max_dist for denormalizing int16 values
    glm::vec3 sdf_max;         // local-space SDF volume max
    uint32_t grid_offset;      // offset into mesh SDF buffer (in int16 units)
    glm::uvec3 grid_resolution;
    uint32_t pad;
};
static_assert(sizeof(GpuSdfInstance) == 112);

class SceneSdfPass {
public:
    static constexpr uint32_t DEFAULT_RESOLUTION = 128;
    static constexpr VkFormat SDF_FORMAT = VK_FORMAT_R16_SFLOAT;

    void create(Device& device, VkDescriptorSetLayout global_set_layout);
    void destroy();

    // Register a model's per-mesh SDF data. Returns true if data was uploaded.
    bool register_model_sdf(const Model& model, uint32_t model_id);

    // Build the instance list from scene objects and dispatch compositing.
    // instances: world transforms + model IDs of all mesh objects with SDF data.
    void composite(VkCommandBuffer cmd, const std::vector<GpuSdfInstance>& instances);

    // Set world-space bounds of the global SDF volume
    void set_bounds(const glm::vec3& min, const glm::vec3& max);

    [[nodiscard]] bool dirty() const { return dirty_; }
    void mark_dirty() { dirty_ = true; }

    [[nodiscard]] VkImageView view() const { return sdf_view_; }
    [[nodiscard]] VkSampler sampler() const { return sdf_sampler_; }
    [[nodiscard]] VkImage image() const { return sdf_image_; }
    [[nodiscard]] const glm::vec3& volume_min() const { return volume_min_; }
    [[nodiscard]] const glm::vec3& volume_max() const { return volume_max_; }
    [[nodiscard]] uint32_t resolution() const { return resolution_; }

    // Debug slice visualization
    void render_slice(VkCommandBuffer cmd, uint32_t axis, float slice_pos);
    [[nodiscard]] VkDescriptorSet slice_imgui_set() const { return slice_imgui_set_; }

    // Per-model SDF metadata
    struct MeshSdfEntry {
        uint32_t offset;      // in int16 units
        uint32_t resolution;
        glm::vec3 sdf_min;
        glm::vec3 sdf_max;
        float max_dist;
    };

    // Lookup a registered model's SDF entry (returns nullptr if not found)
    [[nodiscard]] const MeshSdfEntry* model_entry(uint32_t model_id) const;

private:
    void create_volume_image();
    void create_compute_pipeline();

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};

    // Global SDF 3D texture
    VkImage sdf_image_{VK_NULL_HANDLE};
    VkDeviceMemory sdf_memory_{VK_NULL_HANDLE};
    VkImageView sdf_view_{VK_NULL_HANDLE};
    VkSampler sdf_sampler_{VK_NULL_HANDLE};

    // Compositing compute pipeline
    VkDescriptorSetLayout composite_set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout composite_layout_{VK_NULL_HANDLE};
    VkPipeline composite_pipeline_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSet composite_set_{VK_NULL_HANDLE};

    // Per-mesh SDF data packed into a single SSBO
    std::unique_ptr<Buffer> mesh_sdf_buffer_;
    uint32_t mesh_sdf_offset_{0}; // next free offset in int16 units

    // Per-instance metadata SSBO
    std::unique_ptr<Buffer> instance_buffer_;

    // Volume config
    glm::vec3 volume_min_{-50.0f};
    glm::vec3 volume_max_{50.0f};
    uint32_t resolution_{DEFAULT_RESOLUTION};
    bool dirty_{true};

    std::unordered_map<uint32_t, MeshSdfEntry> model_sdf_entries_;

    // Slice visualization
    static constexpr uint32_t SLICE_SIZE = 256;
    VkImage slice_image_{VK_NULL_HANDLE};
    VkDeviceMemory slice_memory_{VK_NULL_HANDLE};
    VkImageView slice_view_{VK_NULL_HANDLE};
    VkSampler slice_sampler_{VK_NULL_HANDLE};
    VkPipelineLayout slice_layout_{VK_NULL_HANDLE};
    VkPipeline slice_pipeline_{VK_NULL_HANDLE};
    VkDescriptorSetLayout slice_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool slice_pool_{VK_NULL_HANDLE};
    VkDescriptorSet slice_set_{VK_NULL_HANDLE};
    VkDescriptorSet slice_imgui_set_{VK_NULL_HANDLE};
    bool slice_initialized_{false};
};

} // namespace fjell
