#include "renderer/global_bindings.hpp"
#include "core/log.hpp"
// Through the include path: a quoted "gpu/device.hpp" finds the older
// renderer/gpu/device.hpp beside this file first.
#include <gpu/device.hpp>

#include <stdexcept>

namespace fjell {

GlobalBindings::GlobalBindings() {
    // The numbers are the shaders': they declare these bindings by number,
    // so a number never changes.
    using gpu::BindingKind;
    using gpu::ShaderStage;
    constexpr gpu::ShaderStages FRAG = ShaderStage::fragment;
    constexpr gpu::ShaderStages FRAG_COMP = ShaderStage::fragment | ShaderStage::compute;
    constexpr gpu::ShaderStages ALL = ShaderStage::vertex | ShaderStage::fragment | ShaderStage::compute |
                                      ShaderStage::task | ShaderStage::mesh;

    declare({"global_ubo",          0, BindingKind::uniform_buffer,  ALL});
    declare({"shadow_map",          1, BindingKind::sampled_texture, FRAG_COMP});
    declare({"light_ssbo",          2, BindingKind::storage_buffer,  FRAG_COMP});
    declare({"tile_light_ssbo",     3, BindingKind::storage_buffer,  FRAG_COMP});
    declare({"depth_texture",       4, BindingKind::sampled_texture, FRAG_COMP});
    declare({"ao_texture",          5, BindingKind::sampled_texture, FRAG});
    declare({"sky_cubemap",         6, BindingKind::sampled_texture, FRAG_COMP});
    declare({"brdf_lut",            7, BindingKind::sampled_texture, FRAG});
    declare({"screen_color",        8, BindingKind::sampled_texture, FRAG});
    declare({"scene_sdf",           9, BindingKind::sampled_texture, FRAG_COMP});
    declare({"gi_indirect",        10, BindingKind::sampled_texture, FRAG});
    declare({"local_shadow_atlas", 11, BindingKind::sampled_texture, FRAG_COMP});
    declare({"ss_gi_emissive",     12, BindingKind::sampled_texture, FRAG});
    declare({"contact_shadow",     13, BindingKind::sampled_texture, FRAG});
    declare({"environment_ubo",    14, BindingKind::uniform_buffer,  ALL});
    declare({"cloud_shadow",       15, BindingKind::sampled_texture, FRAG_COMP});
    declare({"motion_field",       16, BindingKind::sampled_texture, ALL});
    declare({"fog_volume_ssbo",    17, BindingKind::storage_buffer,  FRAG_COMP});
    declare({"fog_density_field",  18, BindingKind::sampled_texture, FRAG_COMP});
}

void GlobalBindings::declare(Declared decl) {
    if (index_.contains(decl.name)) {
        throw std::runtime_error("GlobalBindings: duplicate binding name '" + decl.name + "'");
    }
    for (const Declared& d : declared_) {
        if (d.binding == decl.binding) {
            throw std::runtime_error("GlobalBindings: binding " + std::to_string(decl.binding) +
                                     " already used by '" + d.name + "', cannot assign to '" +
                                     decl.name + "'");
        }
    }
    index_.emplace(decl.name, declared_.size());
    declared_.push_back(std::move(decl));
}

Result<> GlobalBindings::create(gpu::Device& gpu) {
    gpu_ = &gpu;
    gpu::SharedLayoutDesc desc{.name = "globals", .usual_set = 0, .bindings = {}};
    for (const Declared& d : declared_) {
        gpu::ShaderBinding binding;
        binding.name = d.name;
        binding.binding = d.binding;
        binding.kind = d.kind;
        binding.stages = d.stages;
        desc.bindings.push_back(std::move(binding));
    }
    auto layout = gpu.create(desc);
    if (!layout) return make_error(layout.error());
    layout_ = *layout;
    for (auto& contents : contents_) contents.assign(declared_.size(), std::nullopt);
    return {};
}

std::optional<std::vector<gpu::BindEntry>> GlobalBindings::entries(uint32_t slot) {
    std::vector<gpu::BindEntry> entries;
    entries.reserve(declared_.size());
    for (size_t i = 0; i < declared_.size(); ++i) {
        const std::optional<gpu::BindResource>& content = contents_[slot][i];
        if (!content) {
            if (!missing_reported_) {
                missing_reported_ = true;
                FJELL_GFX_ERROR("Global binding '{}' is bound before anything set it",
                                declared_[i].name);
            }
            return std::nullopt;
        }
        entries.push_back({declared_[i].name, *content});
    }
    return entries;
}

gpu::BindGroup GlobalBindings::group(uint32_t slot) {
    if (groups_[slot]) return groups_[slot].get();
    const auto entries = this->entries(slot);
    if (!entries) return {};
    auto made = gpu_->create(gpu::SharedGroupDesc{.layout = layout_, .entries = *entries, .name = "globals"});
    if (!made) {
        FJELL_GFX_ERROR("Global set: {}", made.error());
        return {};
    }
    groups_[slot] = std::move(*made);
    return groups_[slot].get();
}

void GlobalBindings::set(std::string_view name, const gpu::BindResource& resource) {
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) set(slot, name, resource);
}

void GlobalBindings::set(uint32_t slot, std::string_view name, const gpu::BindResource& resource) {
    const auto it = index_.find(name);
    if (it == index_.end()) {
        FJELL_GFX_ERROR("GlobalBindings: no binding '{}'", name);
        return;
    }
    contents_[slot][it->second] = resource;
    if (!groups_[slot]) return;
    const gpu::BindEntry entry[] = {{name, resource}};
    if (auto written = gpu_->update(groups_[slot], entry); !written) {
        FJELL_GFX_ERROR("Global binding '{}': {}", name, written.error());
    }
}

void GlobalBindings::set(uint32_t slot, std::span<const gpu::BindEntry> entries) {
    for (const gpu::BindEntry& entry : entries) {
        const auto it = index_.find(entry.name);
        if (it == index_.end()) {
            FJELL_GFX_ERROR("GlobalBindings: no binding '{}'", entry.name);
            return;
        }
        contents_[slot][it->second] = entry.resource;
    }
    if (!groups_[slot]) return;
    if (auto written = gpu_->update(groups_[slot], entries); !written) {
        FJELL_GFX_ERROR("Global set: {}", written.error());
    }
}

void GlobalBindings::destroy() {
    for (auto& group : groups_) group.reset();
    for (auto& contents : contents_) contents.clear();
}

} // namespace fjell
