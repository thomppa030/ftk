#include "renderer/pass/depth_variants.hpp"

#include "core/log.hpp"
#include "gpu/device.hpp"
#include "gpu/shader.hpp"

namespace fjell {

Result<> DepthVariants::create(gpu::Device& device, const char* vertex, const char* fragment,
                               const DepthVariantState& state, gpu::SharedLayout globals,
                               gpu::SharedLayout bindless, gpu::SharedLayout draw_data,
                               std::string name) {
    device_ = &device;
    fragment_ = fragment;
    state_ = state;
    globals_ = globals;
    bindless_ = bindless;
    draw_data_ = draw_data;
    name_ = std::move(name);
    FJELL_TRY(default_, device.create(gpu::GraphicsPipelineDesc{
                            .vertex = vertex,
                            .fragment = fragment,
                            .vertex_layout = state.vertex_layout,
                            .raster = state.raster,
                            .depth = state.depth,
                            .depth_format = state.depth_format,
                            .samples = state.samples,
                            .shared = {globals, draw_data},
                            .name = name_,
                        }));
    return {};
}

void DepthVariants::destroy() {
    custom_.clear();
    default_.reset();
}

void DepthVariants::register_custom(uint32_t pipeline_id, const std::string& spirv_path) {
    if (device_ == nullptr) return;
    auto vertex = gpu::read_spirv(spirv_path);
    if (!vertex) {
        FJELL_GFX_ERROR("{}: {}", name_, vertex.error());
        return;
    }
    const std::string name = name_ + " for shader " + std::to_string(pipeline_id);
    auto built = device_->create(gpu::GraphicsPipelineDesc{
        .vertex = std::span<const uint32_t>(*vertex),
        .fragment = fragment_,
        .vertex_layout = state_.vertex_layout,
        .raster = state_.raster,
        .depth = state_.depth,
        .depth_format = state_.depth_format,
        .samples = state_.samples,
        .shared = {globals_, bindless_, draw_data_},
        .name = name,
    });
    if (!built) {
        FJELL_GFX_ERROR("{}: {}", name, built.error());
        return;
    }
    // A frame still drawing with the old one keeps it until it is done.
    custom_[pipeline_id] = std::move(*built);
}

void DepthVariants::clear_custom() {
    custom_.clear();
}

} // namespace fjell
