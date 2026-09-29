#pragma once

#include "gpu/format.hpp"
#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "renderer/resources/fjsl_compiler.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class Device;
class Project;

// Manages the pipelines (transparent, skinned, mesh shader, outline) of every
// unique .fjsl shader, built by the GPU interface's device with layouts read
// from the shaders.
// Pipeline ID 0 is always standard.fjsl (identical to the old mesh.frag). The
// FjslCompiler turns each shader into SPIR-V; this picks the template its
// type is generated into, the variants it is compiled in, and builds the
// pipelines the spatial ones are drawn with.
class PipelineCache {
public:
    /// The scene's targets the pipelines draw to.
    struct Targets {
        gpu::Format color{gpu::Format::undefined};
        gpu::Format mr{gpu::Format::undefined};
        gpu::Format normal{gpu::Format::undefined};
        gpu::Format velocity{gpu::Format::undefined};
        gpu::Format depth{gpu::Format::undefined};
        gpu::Samples samples{gpu::Samples::x1};
    };
    /// The engine's shared layouts the pipelines bind. `meshlets` is unset
    /// without mesh shaders, and no mesh pipeline is built.
    struct SharedLayouts {
        gpu::SharedLayout globals{};
        gpu::SharedLayout bindless{};
        gpu::SharedLayout draw_data{};
        gpu::SharedLayout meshlets{};
    };

    PipelineCache(Device& device, gpu::Device& gpu, const Targets& targets,
                  const SharedLayouts& layouts);
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

    // Each falls back to the standard shader's while an entry has none.
    [[nodiscard]] gpu::GraphicsPipeline transparent_pipeline(uint32_t id) const;
    /// Unset without mesh shaders.
    [[nodiscard]] gpu::GraphicsPipeline gpu_driven_pipeline(uint32_t id) const;
    [[nodiscard]] gpu::GraphicsPipeline skinned_opaque_pipeline(uint32_t id) const;
    [[nodiscard]] gpu::GraphicsPipeline skinned_transparent_pipeline(uint32_t id) const;
    [[nodiscard]] gpu::GraphicsPipeline outline_pipeline(uint32_t id) const;
    [[nodiscard]] const ShaderMetadata& metadata(uint32_t id) const;
    [[nodiscard]] bool has_custom_vertex(uint32_t id) const;
    [[nodiscard]] const std::string& depth_vert_spv(uint32_t id) const;
    [[nodiscard]] const std::string& depth_mesh_spv(uint32_t id) const;
    [[nodiscard]] const std::string& shadow_vert_spv(uint32_t id) const;
    // Path to the compiled fragment SPV. Used by SkyPass and TerrainPass (and
    // any future owner of a custom pipeline shape) to build their own
    // pipeline from an FJSL-generated fragment.
    [[nodiscard]] const std::string& frag_spv_path(uint32_t id) const;
    // A `shader_type ui;` entry compiles its fragment twice, once per UI
    // pipeline shape: screen-space, and worldspace with the scene pass's
    // extra attachments. UiRenderer builds its pipelines from these. Empty
    // for every other shader type.
    [[nodiscard]] const std::string& ui_frag_spv(uint32_t id) const;
    [[nodiscard]] const std::string& ui_world_frag_spv(uint32_t id) const;
    /// shader_type sky: the cubemap compute variant of the sky shader.
    [[nodiscard]] const std::string& sky_cubemap_spv(uint32_t id) const;
    /// shader_type fog: the two compute variants the volumetric fog pass
    /// dispatches, the medium (one source of fog into the accumulated medium)
    /// and the light (the accumulated medium, lit). Empty for every other
    /// type, which compile a fragment instead.
    [[nodiscard]] const std::string& fog_medium_spv(uint32_t id) const;
    [[nodiscard]] const std::string& fog_light_spv(uint32_t id) const;
    // Counts successful compiles of the entry. An owner that built its own
    // pipeline from the entry's SPV compares this against the value it
    // built at and rebuilds when it moved, which is how a hot-reload reaches
    // pipelines the cache does not create itself.
    [[nodiscard]] uint32_t generation(uint32_t id) const { return entries_[id].generation; }
    [[nodiscard]] uint32_t shader_count() const { return static_cast<uint32_t>(entries_.size()); }
    [[nodiscard]] const std::string& compile_error(uint32_t id) const { return entries_[id].compile_error; }
    [[nodiscard]] bool has_compile_error(uint32_t id) const { return !entries_[id].compile_error.empty(); }
    [[nodiscard]] const std::string& shader_path(uint32_t id) const { return entries_[id].program.path; }

    /// The project whose shaders take precedence over the engine's own.
    ///
    /// A material names its shader the same way whichever tree owns it —
    /// `shaders/standard.fjsl` is the engine's, `shaders/water.fjsl` is the
    /// game's — so without a project the cache can only ever find the
    /// engine's, and a game's shader silently resolves to whatever the
    /// engine happens to have at that path. Set this when a project opens,
    /// and clear it when one closes.
    void set_project(const Project* project) { project_ = project; }

    // Recreate all pipelines (e.g. after hot-reload of shared shaders)
    void recreate_all();

    /// Rebuild only the pipelines whose shader is one of `changed_paths`,
    /// each an absolute path from the file watcher. Registered paths are a
    /// mix — engine shaders are registered relative, a project's absolute —
    /// so a path matches when one is a suffix of the other on a path-segment
    /// boundary. Returns how many were rebuilt.
    ///
    /// A shader nothing has registered matches nothing and rebuilds nothing,
    /// which is correct: no pipeline uses it yet.
    uint32_t reload_changed(const std::vector<std::string>& changed_paths);

private:
    struct Entry {
        /// The shader: its path from registration on, the rest as of its last
        /// successful compile. Editing a function it composes has to rebuild
        /// it: the function's code is inlined here, and nothing else would
        /// notice it changed.
        FjslProgram program;
        std::string generated_frag_path;
        bool has_custom_vertex{false};
        gpu::Owned<gpu::GraphicsPipeline> transparent;
        gpu::Owned<gpu::GraphicsPipeline> gpu_driven;
        gpu::Owned<gpu::GraphicsPipeline> skinned_opaque;
        gpu::Owned<gpu::GraphicsPipeline> skinned_transparent;
        gpu::Owned<gpu::GraphicsPipeline> outline;
        std::string frag_spv;  // path to compiled fragment SPIR-V
        // shader_type ui only: the two UI pipeline shapes
        std::string ui_frag_spv;
        std::string ui_world_frag_spv;
        // shader_type sky only: the same source as a compute shader that
        // renders the lighting cubemap
        std::string sky_cubemap_spv;
        // shader_type fog only: the generated source compiled as the two
        // compute variants the fog pass dispatches; frag_spv stays empty
        std::string fog_medium_spv;
        std::string fog_light_spv;
        // Custom vertex SPIR-V paths (empty = use standard shaders)
        std::string vert_spv;
        std::string mesh_spv;
        std::string depth_vert_spv;
        std::string depth_mesh_spv;
        std::string shadow_vert_spv;
        std::string outline_vert_spv;
        // Last compile error (empty = success)
        std::string compile_error;
        // Incremented on every successful compile; 0 until the first one
        uint32_t generation{0};
    };

    /// Read the templates from shaders/include/. Run at construction and
    /// again by recreate_all(), which is what an edited include leads to:
    /// until they are read again the templates are the old text.
    void load_templates();
    void compile_entry(Entry& entry);
    /// Builds the entry's pipelines around the fragment SPIR-V at
    /// `spv_path`; throws with the reason one cannot be built.
    void create_pipelines(Entry& entry, const std::string& spv_path);

    /// Waited on before an entry is compiled again.
    Device& device_;
    gpu::Device& gpu_;
    Targets targets_;
    SharedLayouts layouts_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, uint32_t> path_to_id_;
    std::string template_source_;
    std::string sky_template_;
    std::string ui_template_;
    std::string terrain_template_;
    std::string water_template_;
    std::string fog_template_;
    std::string post_template_;
    std::string mesh_template_;
    std::string depth_mesh_template_;
    std::string vertex_template_;
    std::string depth_vertex_template_;
    std::string shadow_vertex_template_;
    std::string outline_vertex_template_;
    const Project* project_{nullptr};
    /// Finds a shader in the project first, then the engine: read through
    /// `project_` at each lookup, so set_project() is all it takes.
    FjslCompiler compiler_;
};

} // namespace fjell
