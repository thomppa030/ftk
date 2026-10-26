#pragma once

#include "ftk/base/result.hpp"
#include "ftk/gpu/frame.hpp"
#include "ftk/gpu/vulkan/command_list_impl.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ftk::gpu {

class Swapchain;

/// A list a frame handed out, with the recording state behind it.
struct FrameList {
    FrameList(Device& device, VkCommandBuffer cb, Queue queue) noexcept
        : impl{.cb = cb, .queue = queue}, list(device, impl) {}

    FrameList(const FrameList&) = delete;
    FrameList& operator=(const FrameList&) = delete;

    CommandList::Impl impl;
    CommandList list;
};

/// The Vulkan backend's frame: its command buffers, the lists it handed out
/// and the order they go to the GPU in.
struct Frame::Impl {
    /// Graphics, then compute: the order of the arrays below.
    static constexpr size_t QUEUES = 2;

    uint64_t serial{0};
    uint32_t slot{0};
    /// Begun and not yet ended.
    bool open{false};

    /// Per queue, the pool the frame's command buffers come from, reset
    /// when the slot comes round, and the buffers made from it so far, used
    /// again in order after each reset.
    std::array<VkCommandPool, QUEUES> pools{};
    std::array<std::vector<VkCommandBuffer>, QUEUES> buffers;
    std::array<size_t, QUEUES> used{};

    std::vector<std::unique_ptr<FrameList>> lists;

    /// What `parallel_commands` hands out: a pool per thread and queue, since
    /// a pool is used by one thread at a time, made when a thread first
    /// asks and reset with the slot, and the lists made from them.
    struct ThreadPools {
        std::array<VkCommandPool, QUEUES> pools{};
        std::array<std::vector<VkCommandBuffer>, QUEUES> buffers;
        std::array<size_t, QUEUES> used{};
    };
    std::mutex parallel_mutex;
    std::unordered_map<std::thread::id, ThreadPools> parallel_pools;
    std::vector<std::unique_ptr<FrameList>> parallel_lists;

    struct Submission {
        CommandList* list{nullptr};
        const CommandList* after{nullptr};
    };
    std::vector<Submission> submissions;

    /// A swapchain image the frame acquired, shown as it ends.
    struct Acquired {
        Swapchain* swapchain{nullptr};
        uint32_t index{0};
        /// What the list drawing it waits for at colour output.
        VkSemaphore acquired{VK_NULL_HANDLE};
        /// What the frame's last list signals, and present waits on.
        VkSemaphore rendered{VK_NULL_HANDLE};
        /// Taken only when ready (`Swapchain::acquire_ready`): the frame's
        /// last graphics list waits for it, not its first, so it holds
        /// nothing before the list that draws it.
        bool ready_only{false};
        /// A list waits for it already.
        bool waited{false};
    };
    std::vector<Acquired> images;
};

/// One of the device's frames in flight: the interface's frame and its
/// state.
struct FrameSlot {
    explicit FrameSlot(Device& device) noexcept : frame(device, impl) {}

    FrameSlot(const FrameSlot&) = delete;
    FrameSlot& operator=(const FrameSlot&) = delete;

    Frame::Impl impl;
    Frame frame;
};

/// Shows the images `frame` acquired, together in one present once the
/// frame is done, and remakes the swapchains whose windows changed size.
/// Defined with the swapchain.
[[nodiscard]] Result<> present_images(Device& device, Frame::Impl& frame);

} // namespace ftk::gpu
