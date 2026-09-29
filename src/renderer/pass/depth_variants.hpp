#pragma once

#include "core/result.hpp"
#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/render_encoder.hpp"
#include "renderer/indirect_batch.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace fjell {

/// A depth-only pipeline's shaders, named as the build compiled them. A
/// vertex pipeline gives `vertex`; a mesh pipeline gives `task` and `mesh`.
/// A material's own stage takes the place of `vertex` or of `mesh`.
struct DepthVariantShaders {
    const char* vertex{nullptr};
    const char* task{nullptr};
    const char* mesh{nullptr};
    const char* fragment{nullptr};
};

/// What a depth-only pipeline is made of besides its shaders. The raster
/// state's cull is chosen per draw: see DepthVariants.
struct DepthVariantState {
    gpu::VertexLayout vertex_layout{};
    gpu::Raster raster{};
    gpu::Depth depth{};
    gpu::Format depth_format{gpu::Format::undefined};
    gpu::Samples samples{gpu::Samples::x1};
};

/// The engine's shared layouts a depth-only pipeline binds: the globals and
/// the draw data, the meshlets for a mesh pipeline, and the bindless table
/// for a material's own stage, which reads its material.
struct DepthVariantLayouts {
    gpu::SharedLayout globals{};
    gpu::SharedLayout bindless{};
    gpu::SharedLayout draw_data{};
    gpu::SharedLayout meshlets{};
};

/// A depth-only pass's pipelines over the batch's static draws: its own, one
/// for each way a draw can be culled, and one for each material shader with a
/// stage of its own (a displacement), built from that shader's compiled stage
/// on the same state with that material's cull.
class DepthVariants {
public:
    /// The pipeline a range of material shader `pipeline_id` draws with.
    struct Variant {
        gpu::GraphicsPipeline pipeline{};
        /// The material's own stage: its pipeline names the bindless table.
        bool custom{false};
    };

    Result<> create(gpu::Device& device, const DepthVariantShaders& shaders,
                    const DepthVariantState& state, const DepthVariantLayouts& layouts,
                    std::string name);
    void destroy();

    /// Builds, or builds again, the pipeline for material shader
    /// `pipeline_id` from its compiled stage at `spirv_path`, culling `cull`.
    /// A failure is logged, and the shader draws with what it had.
    void register_custom(uint32_t pipeline_id, const std::string& spirv_path, gpu::Cull cull);
    /// Forgets every material shader's pipeline: they draw with the default.
    void clear_custom();

    /// The pipeline material shader `pipeline_id` draws with: its own, or the
    /// pass's culling `cull`.
    [[nodiscard]] Variant variant(uint32_t pipeline_id, gpu::Cull cull) const {
        const auto custom = custom_.find(pipeline_id);
        return custom != custom_.end() ? Variant{custom->second, true}
                                       : Variant{defaults_[static_cast<size_t>(cull)], false};
    }
    [[nodiscard]] bool has_custom(uint32_t pipeline_id) const { return custom_.contains(pipeline_id); }

    /// Draws the batch's static draws range by range, each with its material
    /// shader's pipeline or the pass's culling `cull_of(pipeline_id)`: a
    /// vertex pipeline's indexed draws from the batch's indirect commands.
    /// `bind(custom)` runs after every pipeline change to bind what the
    /// pipeline reads: the bindless table too when `custom`.
    template <typename CullOf, typename Bind>
    void draw_batch(gpu::RenderEncoder& pass, const IndirectBatch& batch, uint32_t frame_index,
                    CullOf&& cull_of, Bind&& bind) const {
        const gpu::Buffer commands = batch.indirect_buffer(frame_index);
        gpu::GraphicsPipeline bound{};
        for (const auto& range : batch.pipeline_ranges(frame_index)) {
            const Variant needed = variant(range.pipeline_id, cull_of(range.pipeline_id));
            if (needed.pipeline != bound) {
                pass.set_pipeline(needed.pipeline);
                bind(needed.custom);
                bound = needed.pipeline;
            }
            pass.draw_indexed_indirect(
                gpu::BufferRange(commands, uint64_t{range.first_draw} * sizeof(gpu::DrawIndexedArgs),
                                 uint64_t{range.draw_count} * sizeof(gpu::DrawIndexedArgs)),
                range.draw_count, sizeof(gpu::DrawIndexedArgs));
        }
    }

private:
    [[nodiscard]] gpu::GraphicsPipelineDesc description(const DepthVariantShaders& shaders, bool custom,
                                                        gpu::Cull cull, std::string_view name) const;

    gpu::Device* device_{nullptr};
    DepthVariantShaders shaders_{};
    DepthVariantState state_{};
    DepthVariantLayouts layouts_{};
    std::string name_;
    /// The pass's own, one per gpu::Cull.
    std::array<gpu::Owned<gpu::GraphicsPipeline>, 3> defaults_;
    std::unordered_map<uint32_t, gpu::Owned<gpu::GraphicsPipeline>> custom_;
};

} // namespace fjell
