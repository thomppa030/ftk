#include "renderer/pass_builder.hpp"

#include "core/log.hpp"

#include <string>

namespace fjell {

FgTexture PassBuilder::create(std::string_view name, const TextureDesc& desc) {
    FgTexture h{next_texture_id_++};
    created_textures_.push_back({
        .handle = h,
        .name = std::string(name),
        .name_hash = fg_name_hash(name),
        .desc = desc,
    });
    return h;
}

FgBuffer PassBuilder::create(std::string_view name, const BufferDesc& desc) {
    FgBuffer h{next_buffer_id_++};
    created_buffers_.push_back({
        .handle = h,
        .name = std::string(name),
        .name_hash = fg_name_hash(name),
        .desc = desc,
    });
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

FgTexture PassBuilder::import_named(const DeclareContext& ctx, std::string_view name) {
    if (!ctx.imports.contains(fg_name_hash(name))) {
        FJELL_GFX_WARN("PassBuilder::import_named: unknown image '{}'",
                       std::string(name));
        return FgTexture{};
    }
    return import_named_optional(ctx, name);
}

FgTexture PassBuilder::import_named_optional(const DeclareContext& ctx, std::string_view name) {
    auto it = ctx.imports.find(fg_name_hash(name));
    if (it == ctx.imports.end()) {
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
        .initial_layout = it->second.initial_layout,
        .persistent = it->second.persistent,
    });
    return h;
}

FgBuffer PassBuilder::import_named_buffer(const DeclareContext& ctx, std::string_view name) {
    auto it = ctx.buffer_imports.find(fg_name_hash(name));
    if (it == ctx.buffer_imports.end() || it->second.buffer == VK_NULL_HANDLE) {
        return FgBuffer{};
    }
    return import(name, it->second.buffer, it->second.size, it->second.persistent);
}

FgBuffer PassBuilder::import(std::string_view name, VkBuffer buffer, VkDeviceSize size,
                              bool persistent) {
    FgBuffer h{next_buffer_id_++};
    imported_buffers_.push_back({
        .handle = h,
        .name = std::string(name),
        .buffer = buffer,
        .size = size,
        .persistent = persistent,
    });
    return h;
}

FgTexture PassBuilder::read(FgTexture h, gpu::Access a) {
    texture_accesses_.push_back({.handle = h, .access = a});
    return h;
}

FgBuffer PassBuilder::read(FgBuffer h, gpu::Access a) {
    if (!h.valid()) { return h; }
    buffer_accesses_.push_back({.handle = h, .access = a});
    return h;
}

FgTexture PassBuilder::write(FgTexture h, gpu::Access a) {
    texture_accesses_.push_back({.handle = h, .access = a});
    return h;
}

FgBuffer PassBuilder::write(FgBuffer h, gpu::Access a) {
    if (!h.valid()) { return h; }
    buffer_accesses_.push_back({.handle = h, .access = a});
    return h;
}

FgTexture PassBuilder::read_write(FgTexture h, gpu::Access a) {
    texture_accesses_.push_back({.handle = h, .access = a});
    return h;
}

FgBuffer PassBuilder::read_write(FgBuffer h, gpu::Access a) {
    if (!h.valid()) { return h; }
    buffer_accesses_.push_back({.handle = h, .access = a});
    return h;
}

FgTexture PassBuilder::final_layout(FgTexture h, VkImageLayout layout,
                                     VkPipelineStageFlags2 written_stage,
                                     VkAccessFlags2 written_access,
                                     VkPipelineStageFlags2 visible_stage,
                                     VkAccessFlags2 visible_access) {
    final_layouts_.push_back({
        .handle = h,
        .layout = layout,
        .written_stage = written_stage,
        .written_access = written_access,
        .visible_stage = visible_stage,
        .visible_access = visible_access,
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
