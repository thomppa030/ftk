#include "gpu/transient_memory.hpp"

#include <algorithm>
#include <string>

namespace fjell::gpu {

TransientMemory::TransientMemory(const Desc& desc, MakeChunk make)
    : slots_(desc.frame_slots), alignment_(desc.alignment), chunk_size_(desc.chunk_size),
      make_(std::move(make)) {}

void TransientMemory::begin_frame(uint32_t slot) {
    std::lock_guard lock(mutex_);
    slot_ = slot;
    Slot& frame = slots_[slot];
    for (Chunk& chunk : frame.chunks) chunk.used = 0;
    frame.current = 0;
}

Result<TransientSlice> TransientMemory::allocate(uint64_t size) {
    if (size == 0) return make_error("Nothing to allocate: a slice of transient memory has a size");

    std::lock_guard lock(mutex_);
    Slot& frame = slots_[slot_];
    for (size_t i = frame.current; i < frame.chunks.size(); ++i) {
        Chunk& chunk = frame.chunks[i];
        const uint64_t offset = (chunk.used + alignment_ - 1) & ~(alignment_ - 1);
        if (offset + size > chunk.memory.bytes.size()) continue;
        chunk.used = offset + size;
        frame.current = i;
        return TransientSlice{{chunk.memory.buffer, offset, size}, chunk.memory.bytes.subspan(offset, size)};
    }

    auto made = make_(std::max(chunk_size_, size));
    if (!made.has_value()) {
        return make_error("No buffer for " + std::to_string(size) + " bytes of transient memory: " +
                          made.error());
    }
    frame.chunks.push_back({*made, size});
    frame.current = frame.chunks.size() - 1;
    return TransientSlice{{made->buffer, 0, size}, made->bytes.first(size)};
}

std::vector<Buffer> TransientMemory::chunks() const {
    std::lock_guard lock(mutex_);
    std::vector<Buffer> all;
    for (const Slot& frame : slots_) {
        for (const Chunk& chunk : frame.chunks) all.push_back(chunk.memory.buffer);
    }
    return all;
}

} // namespace fjell::gpu
