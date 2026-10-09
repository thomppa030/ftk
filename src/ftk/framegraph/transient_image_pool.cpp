#include "ftk/framegraph/transient_image_pool.hpp"

#include "ftk/base/log.hpp"
#include "ftk/gpu/device.hpp"

#include <algorithm>

namespace ftk {

void TransientImagePool::create(gpu::Device& device) {
    device_ = &device;
}

void TransientImagePool::destroy() {
    entries_.clear();
    live_bytes_ = 0;
    device_ = nullptr;
}

bool TransientImagePool::entry_matches(const Entry& e, const TextureDesc& desc,
                                       glm::uvec3 resolved, gpu::TextureUses uses) const {
    // Free once the frame that last took it is complete. A texture never
    // taken (last_used 0) is free.
    if (e.last_used > completed_serial_) { return false; }
    const gpu::TextureInfo& info = device_->info(e.texture);
    return info.format == desc.format
        && info.width == resolved.x
        && info.height == resolved.y
        && info.depth == resolved.z
        && info.mips == desc.mip_levels
        && info.layers == desc.array_layers
        && info.samples == desc.samples
        && info.kind == desc.kind
        // A texture able to do more still serves a request for less; one
        // that lacks a use asked for does not.
        && info.use.has_all(uses);
}

void TransientImagePool::begin_frame(uint64_t frame_serial, uint64_t completed_serial) {
    frame_serial_ = frame_serial;
    completed_serial_ = completed_serial;
    acquires_this_frame_ = 0;
    allocations_this_frame_ = 0;

    // A texture nothing has wanted for STALE_FRAMES belongs to a resolution
    // or a viewport that is gone. Its last frame is long complete, so it
    // can go now.
    std::erase_if(entries_, [&](const Entry& e) {
        if (e.last_used > completed_serial_) { return false; }
        if (frame_serial_ - e.last_used <= STALE_FRAMES) { return false; }
        live_bytes_ -= e.bytes;
        return true;
    });
}

gpu::Texture TransientImagePool::acquire(const TextureDesc& desc, ViewExtents extents, gpu::TextureUses uses) {
    ++acquires_this_frame_;
    const glm::uvec3 resolved = resolved_size(desc, extents);

    for (auto& e : entries_) {
        if (entry_matches(e, desc, resolved, uses)) {
            e.last_used = frame_serial_;
            return e.texture;
        }
    }

    // Miss — make a fresh one.
    ++allocations_this_frame_;
    auto made = device_->create(gpu::TextureDesc{
        .kind = desc.kind,
        .format = desc.format,
        .width = resolved.x,
        .height = resolved.y,
        .depth = resolved.z,
        .layers = desc.array_layers,
        .mips = desc.mip_levels,
        .samples = desc.samples,
        .use = uses | desc.extra_use,
        .name = "frame graph transient",
    });
    if (!made) {
        FTK_GFX_ERROR("TransientImagePool: {}", made.error());
        return {};
    }

    Entry entry{};
    entry.texture = std::move(*made);
    entry.bytes = device_->memory_size(entry.texture);
    entry.last_used = frame_serial_;
    live_bytes_ += entry.bytes;
    entries_.push_back(std::move(entry));
    return entries_.back().texture;
}

uint32_t TransientImagePool::live_images() const noexcept {
    return static_cast<uint32_t>(entries_.size());
}

} // namespace ftk
