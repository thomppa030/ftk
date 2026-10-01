#include "renderer/bindless.hpp"
#include "renderer/resources/texture.hpp"
#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/gpu/device.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include <utility>

namespace fjell {

namespace {

using ftk::gpu::ShaderStage;

// Compute reads the table too: the fog material's `shader_type fog` shader
// inside the volumetric fog dispatch, and the ray traced passes reading the
// material at a hit.
constexpr ftk::gpu::ShaderStages TEXTURE_STAGES = ShaderStage::fragment | ShaderStage::compute;
constexpr ftk::gpu::ShaderStages ROW_STAGES =
    ShaderStage::vertex | ShaderStage::fragment | ShaderStage::mesh | ShaderStage::compute;

ftk::gpu::ShaderBinding table_binding(std::string name, uint32_t binding, ftk::gpu::BindingKind kind,
                                 uint32_t count, ftk::gpu::ShaderStages stages) {
    ftk::gpu::ShaderBinding b;
    b.name = std::move(name);
    b.binding = binding;
    b.kind = kind;
    b.count = count;
    b.array = count > 1;
    b.stages = stages;
    return b;
}

} // namespace

ftk::Result<> BindlessTextures::create(ftk::gpu::Device& gpu) {
    gpu_ = &gpu;

    auto layout = gpu.create(ftk::gpu::SharedLayoutDesc{
        .name = "bindless",
        .usual_set = 1,
        .bindings = {table_binding("textures", 0, ftk::gpu::BindingKind::texture, MAX_TEXTURES,
                                   TEXTURE_STAGES),
                     table_binding("materials", 1, ftk::gpu::BindingKind::storage_buffer, 1, ROW_STAGES),
                     table_binding("params", 2, ftk::gpu::BindingKind::storage_buffer, 1, ROW_STAGES),
                     table_binding("samplers", 3, ftk::gpu::BindingKind::sampler, MAX_SAMPLERS,
                                   TEXTURE_STAGES)},
    });
    if (!layout) return ftk::make_error(layout.error());
    layout_ = *layout;

    // The params buffer is never empty: one zero to start.
    params_.push_back(0.0f);
    for (uint32_t i = 0; i < ftk::MAX_FRAMES_IN_FLIGHT; ++i) {
        Slot& slot = slots_[i];
        auto materials = gpu.create(ftk::gpu::BufferDesc{
            .size = sizeof(GpuMaterial) * MAX_MATERIALS,
            .use = ftk::gpu::BufferUse::storage,
            .memory = ftk::gpu::Memory::upload,
            .name = "bindless_materials",
        });
        auto params = gpu.create(ftk::gpu::BufferDesc{
            .size = INITIAL_PARAMS_CAPACITY * sizeof(float),
            .use = ftk::gpu::BufferUse::storage,
            .memory = ftk::gpu::Memory::upload,
            .name = "bindless_params",
        });
        if (!materials) return ftk::make_error(materials.error());
        if (!params) return ftk::make_error(params.error());
        slot.materials = std::move(*materials);
        slot.params = std::move(*params);
        slot.params_capacity = INITIAL_PARAMS_CAPACITY;
        std::ranges::fill(gpu.mapped(slot.materials), std::byte{0});
        std::memcpy(gpu.mapped(slot.params).data(), params_.data(), sizeof(float));

        auto group = gpu.create(ftk::gpu::SharedGroupDesc{
            .layout = layout_,
            .entries = {{"materials", ftk::gpu::storage(slot.materials)},
                        {"params", ftk::gpu::storage(slot.params)}},
            .name = "bindless",
        });
        if (!group) return ftk::make_error(group.error());
        slot.group = std::move(*group);
    }

    FTK_GFX_DEBUG("Bindless registry created (max {} textures, {} materials)",
                    MAX_TEXTURES, MAX_MATERIALS);
    return {};
}

void BindlessTextures::destroy() {
    for (Slot& slot : slots_) slot = Slot{};
    params_.clear();
    materials_.clear();
    textures_.clear();
    texture_to_slot_.clear();
    pending_textures_ = {};
    samplers_.clear();
    pending_samplers_ = {};
}

void BindlessTextures::Pending::mark(uint32_t i) {
    if (i >= bits.size()) bits.resize(i + 1, 0);
    for (uint32_t slot = 0; slot < ftk::MAX_FRAMES_IN_FLIGHT; ++slot) {
        const auto bit = static_cast<uint8_t>(1u << slot);
        if ((bits[i] & bit) != 0) continue;
        bits[i] |= bit;
        lists[slot].push_back(i);
    }
}

std::vector<uint32_t> BindlessTextures::Pending::take(uint32_t slot) {
    const auto bit = static_cast<uint8_t>(1u << slot);
    for (uint32_t i : lists[slot]) bits[i] &= static_cast<uint8_t>(~bit);
    return std::exchange(lists[slot], {});
}

std::optional<uint32_t> BindlessTextures::next_slot() {
    if (textures_.size() < MAX_TEXTURES) {
        textures_.emplace_back();
        return static_cast<uint32_t>(textures_.size() - 1);
    }
    if (!texture_overflow_reported_) {
        texture_overflow_reported_ = true;
        FTK_GFX_ERROR("Bindless textures full: {} slots in use. Everything added past the "
                        "limit draws with the first texture.", MAX_TEXTURES);
    }
    return std::nullopt;
}

std::optional<uint32_t> BindlessTextures::sampler_place(ftk::gpu::Sampler sampler) {
    if (auto it = std::ranges::find(samplers_, sampler); it != samplers_.end()) {
        return static_cast<uint32_t>(it - samplers_.begin());
    }
    if (samplers_.size() < MAX_SAMPLERS) {
        samplers_.push_back(sampler);
        const auto place = static_cast<uint32_t>(samplers_.size() - 1);
        pending_samplers_.mark(place);
        return place;
    }
    if (!sampler_overflow_reported_) {
        sampler_overflow_reported_ = true;
        FTK_GFX_ERROR("Bindless samplers full: {} in use. Textures read with another draw "
                        "with the first texture.", MAX_SAMPLERS);
    }
    return std::nullopt;
}

uint32_t BindlessTextures::add(const Texture& texture) {
    const auto sampler = sampler_place(texture.sampler());
    if (!sampler) return 0;
    if (auto it = texture_to_slot_.find(texture.texture()); it != texture_to_slot_.end()) {
        return index_of(it->second, *sampler);
    }
    const auto slot = next_slot();
    if (!slot) return 0;
    textures_[*slot] = texture.texture();
    texture_to_slot_[texture.texture()] = *slot;
    pending_textures_.mark(*slot);
    return index_of(*slot, *sampler);
}

uint32_t BindlessTextures::reserve_index(ftk::gpu::Sampler sampler) {
    const auto place = sampler_place(sampler);
    const auto slot = next_slot();
    if (!place || !slot) return 0;
    return index_of(*slot, *place);
}

void BindlessTextures::update_entry(uint32_t index, ftk::gpu::Texture texture) {
    const uint32_t slot = index & ((1u << SLOT_BITS) - 1u);
    if (slot >= textures_.size()) return;
    ftk::gpu::Texture& held = textures_[slot];
    if (held.valid()) texture_to_slot_.erase(held);
    held = texture;
    if (texture.valid()) texture_to_slot_[texture] = slot;
    pending_textures_.mark(slot);
}

uint32_t BindlessTextures::add_material(const GpuMaterial& mat) {
    uint32_t index = static_cast<uint32_t>(materials_.size());
    materials_.push_back(mat);
    for (Slot& slot : slots_) slot.materials_pending = true;
    return index;
}

void BindlessTextures::update_material(uint32_t index, const GpuMaterial& mat) {
    if (index < materials_.size()) {
        materials_[index] = mat;
        for (Slot& slot : slots_) slot.materials_pending = true;
    }
}

uint32_t BindlessTextures::allocate_params(uint32_t count) {
    if (count == 0) return 0;
    uint32_t offset = static_cast<uint32_t>(params_.size());
    params_.resize(params_.size() + count, 0.0f);
    for (Slot& slot : slots_) slot.params_pending = true;
    return offset;
}

void BindlessTextures::write_params(uint32_t offset, const float* data, uint32_t count) {
    if (count == 0) return;
    if (offset + count > params_.size()) {
        params_.resize(offset + count, 0.0f);
    }
    std::memcpy(params_.data() + offset, data, count * sizeof(float));
    for (Slot& slot : slots_) slot.params_pending = true;
}

void BindlessTextures::flush(uint32_t index) {
    FTK_PROFILE_SCOPE_N("bindless_flush");
    Slot& slot = slots_[index];
    std::vector<ftk::gpu::BindEntry> writes;

    // A slot given no texture is emptied, and shaders must not read it.
    for (uint32_t i : pending_textures_.take(index)) {
        writes.push_back({"textures", ftk::gpu::texture(textures_[i]), i});
    }
    for (uint32_t i : pending_samplers_.take(index)) {
        writes.push_back({"samplers", ftk::gpu::sampler(samplers_[i]), i});
    }

    if (slot.materials_pending && !materials_.empty()) {
        // The rows are a growing vector and the buffers behind them are not:
        // copy what fits and say what was dropped.
        const std::size_t count = std::min<std::size_t>(materials_.size(), MAX_MATERIALS);
        if (materials_.size() > MAX_MATERIALS && !material_overflow_reported_) {
            material_overflow_reported_ = true;
            FTK_GFX_ERROR("Bindless materials full: {} rows for {} slots. "
                            "Everything past the limit draws with the wrong "
                            "material.", materials_.size(), MAX_MATERIALS);
        }
        std::memcpy(gpu_->mapped(slot.materials).data(), materials_.data(),
                    count * sizeof(GpuMaterial));
        slot.materials_pending = false;
    }

    if (slot.params_pending) {
        if (params_.size() > slot.params_capacity) {
            // The slot's frames before this one still read the old buffer,
            // which its owner keeps until they are done.
            const uint64_t capacity = params_.size() * 2;
            auto grown = gpu_->create(ftk::gpu::BufferDesc{
                .size = capacity * sizeof(float),
                .use = ftk::gpu::BufferUse::storage,
                .memory = ftk::gpu::Memory::upload,
                .name = "bindless_params",
            });
            if (!grown) {
                FTK_GFX_ERROR("Bindless params could not grow: {}", grown.error());
            } else {
                slot.params = std::move(*grown);
                slot.params_capacity = capacity;
                writes.push_back({"params", ftk::gpu::storage(slot.params)});
            }
        }
        const size_t fits = std::min<size_t>(params_.size(), slot.params_capacity);
        std::memcpy(gpu_->mapped(slot.params).data(), params_.data(), fits * sizeof(float));
        slot.params_pending = false;
    }

    if (!writes.empty()) {
        if (auto written = gpu_->update(slot.group, writes); !written) {
            FTK_GFX_ERROR("Bindless table: {}", written.error());
        }
    }
}

} // namespace fjell
