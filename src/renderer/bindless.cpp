#include "renderer/bindless.hpp"
#include "renderer/resources/texture.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
// Through the include path: a quoted "gpu/device.hpp" finds the older
// renderer/gpu/device.hpp beside this file first.
#include <gpu/device.hpp>

#include <algorithm>
#include <cstring>
#include <span>
#include <string>

namespace fjell {

namespace {

using gpu::ShaderStage;

// Compute reads the table too: the fog material's `shader_type fog` shader
// inside the volumetric fog dispatch, and the ray traced passes reading the
// material at a hit.
constexpr gpu::ShaderStages TEXTURE_STAGES = ShaderStage::fragment | ShaderStage::compute;
constexpr gpu::ShaderStages ROW_STAGES =
    ShaderStage::vertex | ShaderStage::fragment | ShaderStage::mesh | ShaderStage::compute;

gpu::ShaderBinding table_binding(std::string name, uint32_t binding, gpu::BindingKind kind,
                                 uint32_t count, gpu::ShaderStages stages) {
    gpu::ShaderBinding b;
    b.name = std::move(name);
    b.binding = binding;
    b.kind = kind;
    b.count = count;
    b.array = count > 1;
    b.stages = stages;
    return b;
}

} // namespace

Result<> BindlessTextures::create(gpu::Device& gpu) {
    gpu_ = &gpu;

    auto layout = gpu.create(gpu::SharedLayoutDesc{
        .name = "bindless",
        .usual_set = 1,
        .bindings = {table_binding("textures", 0, gpu::BindingKind::sampled_texture, MAX_TEXTURES,
                                   TEXTURE_STAGES),
                     table_binding("materials", 1, gpu::BindingKind::storage_buffer, 1, ROW_STAGES),
                     table_binding("params", 2, gpu::BindingKind::storage_buffer, 1, ROW_STAGES)},
    });
    if (!layout) return make_error(layout.error());
    layout_ = *layout;

    // The params buffer is never empty: one zero to start.
    params_.push_back(0.0f);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        Slot& slot = slots_[i];
        auto materials = gpu.create(gpu::BufferDesc{
            .size = sizeof(GpuMaterial) * MAX_MATERIALS,
            .use = gpu::BufferUse::storage,
            .memory = gpu::Memory::upload,
            .name = "bindless_materials",
        });
        auto params = gpu.create(gpu::BufferDesc{
            .size = INITIAL_PARAMS_CAPACITY * sizeof(float),
            .use = gpu::BufferUse::storage,
            .memory = gpu::Memory::upload,
            .name = "bindless_params",
        });
        if (!materials) return make_error(materials.error());
        if (!params) return make_error(params.error());
        slot.materials = std::move(*materials);
        slot.params = std::move(*params);
        slot.params_capacity = INITIAL_PARAMS_CAPACITY;
        std::ranges::fill(gpu.mapped(slot.materials), std::byte{0});
        std::memcpy(gpu.mapped(slot.params).data(), params_.data(), sizeof(float));

        auto group = gpu.create(gpu::SharedGroupDesc{
            .layout = layout_,
            .entries = {{"materials", gpu::storage(slot.materials)},
                        {"params", gpu::storage(slot.params)}},
            .name = "bindless",
        });
        if (!group) return make_error(group.error());
        slot.group = std::move(*group);
    }

    FJELL_GFX_DEBUG("Bindless registry created (max {} textures, {} materials)",
                    MAX_TEXTURES, MAX_MATERIALS);
    return {};
}

void BindlessTextures::destroy() {
    for (Slot& slot : slots_) slot = Slot{};
    params_.clear();
    materials_.clear();
    entries_.clear();
    entry_pending_.clear();
    texture_to_index_.clear();
}

void BindlessTextures::mark_texture(uint32_t index) {
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        const auto bit = static_cast<uint8_t>(1u << i);
        if ((entry_pending_[index] & bit) != 0) continue;
        entry_pending_[index] |= bit;
        slots_[i].pending_textures.push_back(index);
    }
}

std::optional<uint32_t> BindlessTextures::next_index() {
    if (entries_.size() < MAX_TEXTURES) {
        entries_.emplace_back();
        entry_pending_.push_back(0);
        return static_cast<uint32_t>(entries_.size() - 1);
    }
    if (!texture_overflow_reported_) {
        texture_overflow_reported_ = true;
        FJELL_GFX_ERROR("Bindless textures full: {} slots in use. Everything added past the "
                        "limit draws with the first texture.", MAX_TEXTURES);
    }
    return std::nullopt;
}

uint32_t BindlessTextures::add(const Texture& texture) {
    if (auto it = texture_to_index_.find(texture.texture()); it != texture_to_index_.end()) {
        return it->second;
    }
    const auto index = next_index();
    if (!index) return 0;
    entries_[*index] = {texture.texture(), texture.sampler()};
    texture_to_index_[texture.texture()] = *index;
    mark_texture(*index);
    return *index;
}

uint32_t BindlessTextures::reserve_index() {
    return next_index().value_or(0);
}

void BindlessTextures::update_entry(uint32_t index, gpu::Texture texture, gpu::Sampler sampler) {
    if (index >= entries_.size()) return;
    Entry& entry = entries_[index];
    if (entry.texture.valid()) texture_to_index_.erase(entry.texture);
    entry = {texture, sampler};
    if (texture.valid()) texture_to_index_[texture] = index;
    mark_texture(index);
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
    FJELL_PROFILE_SCOPE_N("bindless_flush");
    Slot& slot = slots_[index];
    std::vector<gpu::BindEntry> writes;

    if (!slot.pending_textures.empty()) {
        const auto bit = static_cast<uint8_t>(1u << index);
        writes.reserve(slot.pending_textures.size() + 1);
        for (uint32_t i : slot.pending_textures) {
            entry_pending_[i] &= static_cast<uint8_t>(~bit);
            // An entry given no texture is emptied, and shaders must not
            // read it.
            const Entry& entry = entries_[i];
            writes.push_back({"textures", gpu::sampled(entry.texture, entry.sampler), i});
        }
        slot.pending_textures.clear();
    }

    if (slot.materials_pending && !materials_.empty()) {
        // The rows are a growing vector and the buffers behind them are not:
        // copy what fits and say what was dropped.
        const std::size_t count = std::min<std::size_t>(materials_.size(), MAX_MATERIALS);
        if (materials_.size() > MAX_MATERIALS && !material_overflow_reported_) {
            material_overflow_reported_ = true;
            FJELL_GFX_ERROR("Bindless materials full: {} rows for {} slots. "
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
            auto grown = gpu_->create(gpu::BufferDesc{
                .size = capacity * sizeof(float),
                .use = gpu::BufferUse::storage,
                .memory = gpu::Memory::upload,
                .name = "bindless_params",
            });
            if (!grown) {
                FJELL_GFX_ERROR("Bindless params could not grow: {}", grown.error());
            } else {
                slot.params = std::move(*grown);
                slot.params_capacity = capacity;
                writes.push_back({"params", gpu::storage(slot.params)});
            }
        }
        const size_t fits = std::min<size_t>(params_.size(), slot.params_capacity);
        std::memcpy(gpu_->mapped(slot.params).data(), params_.data(), fits * sizeof(float));
        slot.params_pending = false;
    }

    if (!writes.empty()) {
        if (auto written = gpu_->update(slot.group, writes); !written) {
            FJELL_GFX_ERROR("Bindless table: {}", written.error());
        }
    }
}

} // namespace fjell
