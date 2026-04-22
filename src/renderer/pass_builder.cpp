#include "renderer/pass_builder.hpp"

#include "core/log.hpp"

#include <string>

namespace fjell {

namespace {
// FNV-1a over a string_view. Used to key transient resources by a cheap
// 64-bit hash instead of the string itself — the DAG machinery does a
// lot of (writer/reader) lookups per frame, and the strings are all
// short (`"bloom_mip_0"`, `"gtao_ao"`...) so collisions among ~20 names
// are astronomically unlikely.
constexpr uint64_t fnv1a64(std::string_view s) noexcept {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char b : s) {
        h ^= static_cast<uint64_t>(b);
        h *= 1099511628211ULL;
    }
    return h;
}
} // anonymous

FgTexture PassBuilder::create(std::string_view name, const TextureDesc& desc) {
    FgTexture h{next_texture_id_++};
    created_textures_.push_back({
        .handle = h,
        .name = std::string(name),
        .name_hash = fnv1a64(name),
        .desc = desc,
    });
    return h;
}

FgBuffer PassBuilder::create(std::string_view name, const BufferDesc& desc) {
    FgBuffer h{next_buffer_id_++};
    created_buffers_.push_back({.handle = h, .name = std::string(name), .desc = desc});
    return h;
}

FgTexture PassBuilder::import(std::string_view name, VkImage image, VkImageView view,
                                   VkImageAspectFlags aspect,
                                   uint32_t base_layer, uint32_t layer_count,
                                   VkImageLayout initial_layout) {
    FgTexture h{next_texture_id_++};
    imported_textures_.push_back({
        .handle = h,
        .name = std::string(name),
        .image = image,
        .view = view,
        .aspect = aspect,
        .base_layer = base_layer,
        .layer_count = layer_count,
        .mip_count = 1,
        .initial_layout = initial_layout,
        .persistent = false,
    });
    return h;
}

FgTexture PassBuilder::import_named(const DeclareContext& ctx, std::string_view name,
                                     VkImageLayout initial_layout) {
    std::string key(name);
    auto it = ctx.imports.find(key);
    if (it == ctx.imports.end()) {
        FJELL_GFX_WARN("PassBuilder::import_named: unknown image '{}'", key.c_str());
        return FgTexture{};
    }
    FgTexture h{next_texture_id_++};
    imported_textures_.push_back({
        .handle = h,
        .name = std::string(name),
        .image = it->second.image,
        .view = it->second.view,
        .aspect = it->second.aspect,
        .base_layer = it->second.base_layer,
        .layer_count = it->second.layer_count,
        .mip_count = it->second.mip_count,
        .initial_layout = initial_layout,
        .persistent = it->second.persistent,
    });
    return h;
}

FgBuffer PassBuilder::import(std::string_view name, VkBuffer buffer, VkDeviceSize size) {
    FgBuffer h{next_buffer_id_++};
    imported_buffers_.push_back({
        .handle = h,
        .name = std::string(name),
        .buffer = buffer,
        .size = size,
    });
    return h;
}

FgTexture PassBuilder::read(FgTexture h, ResourceAccess a) {
    texture_accesses_.push_back({.handle = h, .access = a});
    return h;
}

FgBuffer PassBuilder::read(FgBuffer h, ResourceAccess a) {
    buffer_accesses_.push_back({.handle = h, .access = a});
    return h;
}

FgTexture PassBuilder::write(FgTexture h, ResourceAccess a) {
    texture_accesses_.push_back({.handle = h, .access = a});
    return h;
}

FgBuffer PassBuilder::write(FgBuffer h, ResourceAccess a) {
    buffer_accesses_.push_back({.handle = h, .access = a});
    return h;
}

FgTexture PassBuilder::read_write(FgTexture h, ResourceAccess a) {
    texture_accesses_.push_back({.handle = h, .access = a});
    return h;
}

FgBuffer PassBuilder::read_write(FgBuffer h, ResourceAccess a) {
    buffer_accesses_.push_back({.handle = h, .access = a});
    return h;
}

FgTexture PassBuilder::final_layout(FgTexture h, VkImageLayout layout,
                                     VkPipelineStageFlags2 last_stage,
                                     VkAccessFlags2 last_access) {
    final_layouts_.push_back({
        .handle = h,
        .layout = layout,
        .last_stage = last_stage,
        .last_access = last_access,
    });
    return h;
}

void PassBuilder::reset() {
    texture_accesses_.clear();
    buffer_accesses_.clear();
    imported_textures_.clear();
    imported_buffers_.clear();
    created_textures_.clear();
    created_buffers_.clear();
    final_layouts_.clear();
    queue_ = QueueType::graphics;
    parallel_group_ = 0;
    never_cull_ = false;
    side_effects_ = false;
    next_texture_id_ = 0;
    next_buffer_id_ = 0;
}

} // namespace fjell
