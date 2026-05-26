#pragma once

#include "renderer/resources/fjsl_parser.hpp"

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
                  VkDescriptorSetLayout meshlet_layout = VK_NULL_HANDLE,
                  VkPipelineCache vk_cache = VK_NULL_HANDLE);
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
    [[nodiscard]] VkPipeline gpu_driven_pipeline(uint32_t id) const;
    [[nodiscard]] VkPipeline skinned_opaque_pipeline(uint32_t id) const;
    [[nodiscard]] VkPipeline skinned_transparent_pipeline(uint32_t id) const;
    [[nodiscard]] VkPipeline outline_pipeline(uint32_t id) const;
    [[nodiscard]] VkPipelineLayout outline_layout() const { return outline_layout_; }
    [[nodiscard]] const ShaderMetadata& metadata(uint32_t id) const;
    [[nodiscard]] bool has_custom_vertex(uint32_t id) const;
    [[nodiscard]] const std::string& depth_vert_spv(uint32_t id) const;
    [[nodiscard]] const std::string& depth_mesh_spv(uint32_t id) const;
    [[nodiscard]] const std::string& shadow_vert_spv(uint32_t id) const;
    [[nodiscard]] const std::string& outline_vert_spv(uint32_t id) const;
    // Path to the compiled fragment SPV. Used by SkyPass (and any future
    // owner of a custom pipeline shape) to build its own VkPipeline from
    // an FJSL-generated fragment.
    [[nodiscard]] const std::string& frag_spv_path(uint32_t id) const;
    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }
    [[nodiscard]] VkPipelineLayout gpu_driven_layout() const { return gpu_driven_layout_; }
    [[nodiscard]] bool has_mesh_shaders() const { return meshlet_layout_ != VK_NULL_HANDLE; }
    [[nodiscard]] uint32_t shader_count() const { return static_cast<uint32_t>(entries_.size()); }
    [[nodiscard]] const std::string& compile_error(uint32_t id) const { return entries_[id].compile_error; }
    [[nodiscard]] bool has_compile_error(uint32_t id) const { return !entries_[id].compile_error.empty(); }
    [[nodiscard]] const std::string& shader_path(uint32_t id) const { return entries_[id].fjsl_path; }

    // Recreate all pipelines (e.g. after hot-reload of shared shaders)
    void recreate_all();

private:
    struct Entry {
        std::string fjsl_path;
        std::string generated_frag_path;
        ShaderMetadata metadata;
        bool has_custom_vertex{false};
        VkPipeline opaque{VK_NULL_HANDLE};
        VkPipeline transparent{VK_NULL_HANDLE};
        VkPipeline gpu_driven{VK_NULL_HANDLE};
        VkPipeline skinned_opaque{VK_NULL_HANDLE};
        VkPipeline skinned_transparent{VK_NULL_HANDLE};
        VkPipeline outline{VK_NULL_HANDLE};
        std::string frag_spv;  // path to compiled fragment SPIR-V
        // Custom vertex SPIR-V paths (empty = use standard shaders)
        std::string vert_spv;
        std::string mesh_spv;
        std::string depth_vert_spv;
        std::string depth_mesh_spv;
        std::string shadow_vert_spv;
        std::string outline_vert_spv;
        // Last compile error (empty = success)
        std::string compile_error;
    };

    void create_layout();
    void compile_entry(Entry& entry);
    void create_pipelines(Entry& entry, const std::string& spv_path);
    void destroy_entry_pipelines(Entry& entry);

    // Compile a GLSL source to SPIR-V using glslc. Returns path to .spv file.
    [[nodiscard]] std::string compile_glsl(const std::string& source_path,
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
    VkPipelineLayout gpu_driven_layout_{VK_NULL_HANDLE};
    VkPipelineLayout outline_layout_{VK_NULL_HANDLE};
    std::vector<Entry> entries_;
    std::unordered_map<std::string, uint32_t> path_to_id_;
    std::string template_source_;
    std::string sky_template_;
    std::string mesh_template_;
    std::string depth_mesh_template_;
    std::string vertex_template_;
    std::string depth_vertex_template_;
    std::string shadow_vertex_template_;
    std::string outline_vertex_template_;
    std::string shader_source_dir_;
    std::string generated_dir_;
    VkPipelineCache vk_cache_{VK_NULL_HANDLE};
};

} // namespace fjell
