#pragma once

#include "renderer/fjsl_parser.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class Device;

// Manages one VkPipeline (opaque + transparent + mesh shader) per unique .fjsl shader.
// All pipelines share the same VkPipelineLayout — only the fragment module differs.
// Pipeline ID 0 is always standard.fjsl (identical to the old mesh.frag).
class PipelineCache {
public:
    PipelineCache(Device& device,
                  VkFormat color_format, VkFormat depth_format,
                  VkDescriptorSetLayout global_layout,
                  VkDescriptorSetLayout material_layout,
                  VkDescriptorSetLayout draw_data_layout,
                  VkSampleCountFlagBits samples,
                  VkDescriptorSetLayout meshlet_layout = VK_NULL_HANDLE);
    ~PipelineCache();

    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;
    PipelineCache(PipelineCache&&) = delete;
    PipelineCache& operator=(PipelineCache&&) = delete;

    // Register a .fjsl shader and compile its pipeline. Returns pipeline ID.
    // If already registered, returns existing ID.
    uint32_t register_shader(const std::string& fjsl_path);

    // Reload a shader (hot-reload). Recompiles and recreates pipeline.
    void reload(uint32_t id);

    [[nodiscard]] VkPipeline opaque_pipeline(uint32_t id) const;
    [[nodiscard]] VkPipeline transparent_pipeline(uint32_t id) const;
    [[nodiscard]] VkPipeline mesh_pipeline(uint32_t id) const;
    [[nodiscard]] VkPipeline gpu_driven_pipeline(uint32_t id) const;
    [[nodiscard]] const ShaderMetadata& metadata(uint32_t id) const;
    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }
    [[nodiscard]] VkPipelineLayout mesh_layout() const { return mesh_layout_; }
    [[nodiscard]] VkPipelineLayout gpu_driven_layout() const { return gpu_driven_layout_; }
    [[nodiscard]] bool has_mesh_shaders() const { return meshlet_layout_ != VK_NULL_HANDLE; }
    [[nodiscard]] uint32_t shader_count() const { return static_cast<uint32_t>(entries_.size()); }

    // Recreate all pipelines (e.g. after hot-reload of shared shaders)
    void recreate_all();

private:
    struct Entry {
        std::string fjsl_path;
        std::string generated_frag_path; // path to generated .frag in build dir
        ShaderMetadata metadata;
        VkPipeline opaque{VK_NULL_HANDLE};
        VkPipeline transparent{VK_NULL_HANDLE};
        VkPipeline mesh{VK_NULL_HANDLE};
        VkPipeline gpu_driven{VK_NULL_HANDLE};
    };

    void create_layout();
    void compile_entry(Entry& entry);
    void create_pipelines(Entry& entry, const std::string& spv_path);
    void destroy_entry_pipelines(Entry& entry);

    // Compile a .frag source to SPIR-V using glslc. Returns path to .spv file.
    [[nodiscard]] std::string compile_glsl(const std::string& frag_source,
                                            const std::string& output_name);

    Device& device_;
    VkFormat color_format_;
    VkFormat depth_format_;
    VkDescriptorSetLayout global_layout_;
    VkDescriptorSetLayout material_layout_;
    VkDescriptorSetLayout draw_data_layout_;
    VkDescriptorSetLayout meshlet_layout_;
    VkSampleCountFlagBits samples_;

    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipelineLayout mesh_layout_{VK_NULL_HANDLE};
    VkPipelineLayout gpu_driven_layout_{VK_NULL_HANDLE};
    std::vector<Entry> entries_;
    std::unordered_map<std::string, uint32_t> path_to_id_;
    std::string template_source_;
    std::string shader_source_dir_;
    std::string generated_dir_;
};

} // namespace fjell
