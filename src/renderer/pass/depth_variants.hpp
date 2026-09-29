#pragma once

#include "core/result.hpp"
#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/render_encoder.hpp"
#include "renderer/indirect_batch.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace fjell {

/// What a depth-only pipeline is made of besides its shaders.
struct DepthVariantState {
    gpu::VertexLayout vertex_layout{};
    gpu::Raster raster{};
    gpu::Depth depth{};
    gpu::Format depth_format{gpu::Format::undefined};
    gpu::Samples samples{gpu::Samples::x1};
};

/// A depth-only pass's vertex pipelines over the batch's static draws: its
/// own default, and one for each material shader with a vertex stage of its
/// own (a displacement), built from that shader's compiled depth or shadow
/// vertex stage on the same state. The default shaders read the globals and
/// the draw data; a material's vertex stage reads its material too, so its
/// pipelines also name the bindless table.
class DepthVariants {
public:
    /// `vertex` and `fragment` name shaders the build compiled.
    Result<> create(gpu::Device& device, const char* vertex, const char* fragment,
                    const DepthVariantState& state, gpu::SharedLayout globals,
                    gpu::SharedLayout bindless, gpu::SharedLayout draw_data, std::string name);
    void destroy();

    /// Builds, or builds again, the pipeline for material shader
    /// `pipeline_id` from its compiled vertex stage at `spirv_path`. A
    /// failure is logged, and the shader draws with what it had.
    void register_custom(uint32_t pipeline_id, const std::string& spirv_path);
    /// Forgets every material shader's pipeline: they draw with the default.
    void clear_custom();

    /// Draws the batch's static draws range by range, each with its material
    /// shader's pipeline or the default. `bind(custom)` runs after every
    /// pipeline change to bind what the pipeline reads: the bindless table
    /// too when `custom`.
    template <typename Bind>
    void draw_batch(gpu::RenderEncoder& pass, const IndirectBatch& batch, uint32_t frame_index,
                    Bind&& bind) const {
        const gpu::Buffer commands = batch.shared_indirect_buffer(frame_index);
        gpu::GraphicsPipeline bound{};
        for (const auto& range : batch.pipeline_ranges(frame_index)) {
            const auto custom = custom_.find(range.pipeline_id);
            const bool is_custom = custom != custom_.end();
            const gpu::GraphicsPipeline needed = is_custom ? custom->second.get() : default_.get();
            if (needed != bound) {
                pass.set_pipeline(needed);
                bind(is_custom);
                bound = needed;
            }
            pass.draw_indexed_indirect(
                gpu::BufferRange(commands, uint64_t{range.first_draw} * sizeof(gpu::DrawIndexedArgs),
                                 uint64_t{range.draw_count} * sizeof(gpu::DrawIndexedArgs)),
                range.draw_count, sizeof(gpu::DrawIndexedArgs));
        }
    }

private:
    gpu::Device* device_{nullptr};
    const char* fragment_{nullptr};
    DepthVariantState state_{};
    gpu::SharedLayout globals_{};
    gpu::SharedLayout bindless_{};
    gpu::SharedLayout draw_data_{};
    std::string name_;
    gpu::Owned<gpu::GraphicsPipeline> default_;
    std::unordered_map<uint32_t, gpu::Owned<gpu::GraphicsPipeline>> custom_;
};

} // namespace fjell
