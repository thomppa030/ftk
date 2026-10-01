#include "renderer/pass/depth_variants.hpp"

#include "ftk/base/log.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/shader.hpp"

namespace fjell {

ftk::gpu::GraphicsPipelineDesc DepthVariants::description(const DepthVariantShaders& shaders, bool custom,
                                                     ftk::gpu::Cull cull, std::string_view name) const {
    ftk::gpu::Raster raster = state_.raster;
    raster.cull = cull;
    ftk::gpu::GraphicsPipelineDesc desc{
        .vertex_layout = state_.vertex_layout,
        .raster = raster,
        .depth = state_.depth,
        .depth_format = state_.depth_format,
        .samples = state_.samples,
        .name = name,
    };
    if (shaders.vertex != nullptr) desc.vertex = shaders.vertex;
    if (shaders.task != nullptr) desc.task = shaders.task;
    if (shaders.mesh != nullptr) desc.mesh = shaders.mesh;
    if (shaders.fragment != nullptr) desc.fragment = shaders.fragment;
    desc.shared = {layouts_.globals, layouts_.draw_data};
    if (shaders.mesh != nullptr) desc.shared.push_back(layouts_.meshlets);
    if (custom) desc.shared.push_back(layouts_.bindless);
    return desc;
}

ftk::Result<> DepthVariants::create(ftk::gpu::Device& device, const DepthVariantShaders& shaders,
                               const DepthVariantState& state, const DepthVariantLayouts& layouts,
                               std::string name) {
    device_ = &device;
    shaders_ = shaders;
    state_ = state;
    layouts_ = layouts;
    name_ = std::move(name);
    for (const ftk::gpu::Cull cull : {ftk::gpu::Cull::none, ftk::gpu::Cull::back, ftk::gpu::Cull::front}) {
        FTK_TRY(defaults_[static_cast<size_t>(cull)], device.create(description(shaders_, false, cull, name_)));
    }
    return {};
}

void DepthVariants::destroy() {
    custom_.clear();
    for (auto& pipeline : defaults_) pipeline.reset();
}

void DepthVariants::register_custom(uint32_t pipeline_id, const std::string& spirv_path,
                                    ftk::gpu::Cull cull) {
    if (device_ == nullptr) return;
    auto stage = ftk::gpu::read_spirv(spirv_path);
    if (!stage) {
        FTK_GFX_ERROR("{}: {}", name_, stage.error());
        return;
    }
    const std::string name = name_ + " for shader " + std::to_string(pipeline_id);
    ftk::gpu::GraphicsPipelineDesc desc = description(shaders_, true, cull, name);
    // The material's stage in place of the pass's own.
    if (shaders_.mesh != nullptr) {
        desc.mesh = std::span<const uint32_t>(*stage);
    } else {
        desc.vertex = std::span<const uint32_t>(*stage);
    }
    auto built = device_->create(desc);
    if (!built) {
        FTK_GFX_ERROR("{}: {}", name, built.error());
        return;
    }
    // A frame still drawing with the old one keeps it until it is done.
    custom_[pipeline_id] = std::move(*built);
}

void DepthVariants::clear_custom() {
    custom_.clear();
}

} // namespace fjell
