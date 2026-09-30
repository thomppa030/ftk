#pragma once

#include "gpu/buffer.hpp"
#include "gpu/queue.hpp"
#include "gpu/transient_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <type_traits>

namespace fjell::gpu {

class CommandList;
class Device;

/// How a list is submitted within its frame.
struct SubmitDesc {
    /// A list of the same frame, submitted before this one, whose work this
    /// one waits for. Lists on one queue run in the order they are submitted;
    /// this is what orders a list after one on the other queue.
    const CommandList* after{nullptr};
};

/// One frame: the lists it records, the order they are submitted in, and
/// the serial what is released while it records waits for.
/// `Device::begin_frame()` starts one and `Device::end_frame()` sends what it
/// submitted to the GPU. The frame belongs to the device and is reused when
/// its slot comes round again.
///
/// @code
/// auto& frame = device.begin_frame();
/// auto& pre = frame.commands(gpu::Queue::graphics);
/// auto& async = frame.commands(gpu::Queue::compute);
/// auto& post = frame.commands(gpu::Queue::graphics);
/// ... record ...
/// frame.submit(pre);
/// frame.submit(async, {.after = &pre});
/// frame.submit(post, {.after = &async});
/// device.end_frame(frame);
/// @endcode
class Frame {
public:
    /// The backend's state for a frame, which the backend defines.
    struct Impl;

    Frame(Device& device, Impl& impl) noexcept : device_(&device), impl_(&impl) {}

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    /// Frames count up from 1; the GPU finishing a frame is what releases
    /// what was released while it recorded.
    [[nodiscard]] uint64_t serial() const noexcept;

    /// Which of the device's frames in flight this is, for arrays kept one
    /// per frame in flight.
    [[nodiscard]] uint32_t slot() const noexcept;

    /// A new list recording for `queue` in this frame, from the frame's own
    /// memory: it lasts until the frame's slot comes round again. The
    /// compute queue needs `Caps::async_compute`.
    [[nodiscard]] CommandList& commands(Queue queue);

    /// Queues `list` to go to the GPU when the frame ends, after the lists
    /// submitted before it on its queue and after `desc.after`. A list that
    /// is never submitted records for nothing.
    void submit(CommandList& list, const SubmitDesc& desc = {});

    /// `size` bytes of the frame's memory for the CPU to fill before the
    /// frame ends, and the range to bind them as: the memory
    /// `CommandList::transient` copies into, which lasts until the frame's
    /// slot comes round again. For data a view writes before anything
    /// records. Empty when no memory could be had, which is reported and
    /// refused where bound.
    [[nodiscard]] TransientSlice transient_slice(uint64_t size);

    /// A copy of `value` in the frame's memory.
    template <typename T>
        requires(std::is_trivially_copyable_v<T> && !std::ranges::range<T>)
    [[nodiscard]] BufferRange transient(const T& value) {
        return transient_bytes(std::as_bytes(std::span(&value, 1)));
    }

    /// A copy of `values`, one after another, in the frame's memory.
    template <std::ranges::contiguous_range R>
        requires std::is_trivially_copyable_v<std::ranges::range_value_t<R>>
    [[nodiscard]] BufferRange transient(const R& values) {
        const std::span all(std::ranges::data(values), std::ranges::size(values));
        return transient_bytes(std::as_bytes(all));
    }

    [[nodiscard]] Impl& impl() const noexcept { return *impl_; }

private:
    [[nodiscard]] BufferRange transient_bytes(std::span<const std::byte> bytes);

    Device* device_;
    Impl* impl_;
};

} // namespace fjell::gpu
