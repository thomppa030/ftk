#pragma once

#include "ftk/base/result.hpp"
#include "ftk/gpu/binding.hpp"
#include "ftk/gpu/buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

namespace ftk::gpu {

/// A buffer transient memory hands out slices of, and the CPU's view of it.
struct TransientChunk {
    Buffer buffer{};
    /// Bytes the buffer holds.
    uint64_t size{0};
    /// The CPU's view of them; empty for memory only the GPU sees.
    std::span<std::byte> bytes{};
};

/// A slice of transient memory: the range to bind, and its bytes for the CPU
/// to fill before the frame is submitted (none in memory only the GPU sees).
struct TransientSlice {
    BufferRange range{};
    std::span<std::byte> bytes{};
};

/// Memory that lasts one frame, what `cmd.transient` copies into: uniforms,
/// storage, vertices, indices and indirect arguments written by the CPU each
/// frame; and, in memory only the GPU sees, what acceleration structure
/// builds work in. Each frame slot keeps the buffers ("chunks") it has needed and hands
/// out aligned slices of them in order; when the slot comes round again, the
/// GPU being done with its last frame, it hands them out anew from the start.
/// A slice larger than a chunk gets a chunk of its own size. Chunks are kept
/// until the owner destroys them, so a slot holds what its busiest frame
/// needed.
///
/// Any thread may allocate; each call takes a lock.
class TransientMemory {
public:
    /// Makes a chunk of at least `size` bytes, or says why it cannot.
    /// The owner keeps what it makes and destroys it (`chunks()`).
    using MakeChunk = std::function<Result<TransientChunk>(uint64_t size)>;

    struct Desc {
        /// The frames recorded ahead of the GPU.
        uint32_t frame_slots{1};
        /// Where every slice starts: a multiple of this, a power of two that
        /// serves every use a slice is bound as.
        uint64_t alignment{16};
        /// Bytes of each chunk made for slices that fit in one.
        uint64_t chunk_size{0};
    };

    TransientMemory(const Desc& desc, MakeChunk make);

    TransientMemory(const TransientMemory&) = delete;
    TransientMemory& operator=(const TransientMemory&) = delete;

    /// Starts `slot`'s frame, once the GPU is done with that slot's last one:
    /// what it handed out then is free again.
    void begin_frame(uint32_t slot);

    /// `size` bytes from the frame slot being recorded, valid until that slot
    /// comes round again.
    /// @return the slice, or why none could be had (nothing asked for, or no
    ///         chunk could be made).
    [[nodiscard]] Result<TransientSlice> allocate(uint64_t size);

    /// Every chunk made, in every slot.
    [[nodiscard]] std::vector<Buffer> chunks() const;

private:
    struct Chunk {
        TransientChunk memory;
        uint64_t used{0};
    };
    struct Slot {
        std::vector<Chunk> chunks;
        /// The chunk slices come from; those before it are spent.
        size_t current{0};
    };

    std::vector<Slot> slots_;
    uint32_t slot_{0};
    uint64_t alignment_;
    uint64_t chunk_size_;
    MakeChunk make_;
    mutable std::mutex mutex_;
};

} // namespace ftk::gpu
