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

FgTexture PassBuilder::import(std::string_view name, const gpu::TextureView& view) {
    FgTexture h{next_texture_id_++};
    imported_textures_.push_back({
        .handle = h,
        .name = std::string(name),
        .view = view,
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
        .view = it->second.view,
        .persistent = it->second.persistent,
        .resting = it->second.resting,
        .unwritten = it->second.unwritten,
    });
    return h;
}

FgBuffer PassBuilder::import_named_buffer(const DeclareContext& ctx, std::string_view name) {
    auto it = ctx.buffer_imports.find(fg_name_hash(name));
    if (it == ctx.buffer_imports.end() || !it->second.buffer.valid()) {
        return FgBuffer{};
    }
    return import(name, it->second.buffer, it->second.persistent);
}

FgBuffer PassBuilder::import(std::string_view name, gpu::Buffer buffer, bool persistent) {
    FgBuffer h{next_buffer_id_++};
    imported_buffers_.push_back({
        .handle = h,
        .name = std::string(name),
        .buffer = buffer,
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

FgTexture PassBuilder::leaves(FgTexture h, gpu::AccessSet written_by, gpu::AccessSet left_as) {
    final_states_.push_back({.handle = h, .written_by = written_by, .left_as = left_as});
    return h;
}

void PassBuilder::reset() {
    texture_accesses_.clear();
    buffer_accesses_.clear();
    imported_textures_.clear();
    imported_buffers_.clear();
    created_textures_.clear();
    created_buffers_.clear();
    final_states_.clear();
    queue_ = QueueType::graphics;
    parallel_group_ = 0;
    never_cull_ = false;
    side_effects_ = false;
    next_texture_id_ = 0;
    next_buffer_id_ = 0;
}

} // namespace fjell
