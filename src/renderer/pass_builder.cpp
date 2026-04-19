#include "renderer/pass_builder.hpp"

#include "core/log.hpp"

#include <string>

namespace fjell {

FgTexture PassBuilder::create(std::string_view name, const TextureDesc& desc) {
    FgTexture h{next_texture_id_++};
    created_textures_.push_back({.handle = h, .name = std::string(name), .desc = desc});
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
        .initial_layout = initial_layout,
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
    return import(name, it->second.image, it->second.view, it->second.aspect,
                  it->second.base_layer, it->second.layer_count, initial_layout);
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

void PassBuilder::reset() {
    texture_accesses_.clear();
    buffer_accesses_.clear();
    imported_textures_.clear();
    imported_buffers_.clear();
    created_textures_.clear();
    created_buffers_.clear();
    queue_ = QueueType::graphics;
    parallel_group_ = 0;
    never_cull_ = false;
    side_effects_ = false;
    next_texture_id_ = 0;
    next_buffer_id_ = 0;
}

} // namespace fjell
