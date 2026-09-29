#include "renderer/pass/depth_variants.hpp"

#include "core/log.hpp"
#include "gpu/device.hpp"
#include "gpu/shader.hpp"

namespace fjell {

gpu::GraphicsPipelineDesc DepthVariants::description(const DepthVariantShaders& shaders, bool custom,
                                                     gpu::Cull cull, std::string_view name) const {
    gpu::Raster raster = state_.raster;
    raster.cull = cull;
    gpu::GraphicsPipelineDesc desc{
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

Result<> DepthVariants::create(gpu::Device& device, const DepthVariantShaders& shaders,
                               const DepthVariantState& state, const DepthVariantLayouts& layouts,
                               std::string name) {
    device_ = &device;
    shaders_ = shaders;
    state_ = state;
    layouts_ = layouts;
    name_ = std::move(name);
    for (const gpu::Cull cull : {gpu::Cull::none, gpu::Cull::back, gpu::Cull::front}) {
        FJELL_TRY(defaults_[static_cast<size_t>(cull)], device.create(description(shaders_, false, cull, name_)));
    }
    return {};
}

void DepthVariants::destroy() {
    custom_.clear();
    for (auto& pipeline : defaults_) pipeline.reset();
}

void DepthVariants::register_custom(uint32_t pipeline_id, const std::string& spirv_path,
                                    gpu::Cull cull) {
    if (device_ == nullptr) return;
    auto stage = gpu::read_spirv(spirv_path);
    if (!stage) {
        FJELL_GFX_ERROR("{}: {}", name_, stage.error());
        return;
    }
    const std::string name = name_ + " for shader " + std::to_string(pipeline_id);
    gpu::GraphicsPipelineDesc desc = description(shaders_, true, cull, name);
    // The material's stage in place of the pass's own.
    if (shaders_.mesh != nullptr) {
        desc.mesh = std::span<const uint32_t>(*stage);
    } else {
        desc.vertex = std::span<const uint32_t>(*stage);
    }
    auto built = device_->create(desc);
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
