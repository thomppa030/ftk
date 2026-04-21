#pragma once

#include "renderer/gpu/device.hpp"
#include "renderer/gpu/swapchain.hpp"
#include "renderer/renderer_constants.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

namespace fjell {

class Window;

/// Core Vulkan infrastructure: device, allocator, swapchain, command pool,
/// command buffers, and frame sync objects. Owns the destruction order so
/// VMA-backed images (e.g. swapchain depth) are freed before the allocator.
///
/// Used directly by lightweight tool windows (hub, file browser) and composed
/// by GpuContext for the full renderer.
class GpuCore {
public:
    explicit GpuCore(Window& window);
    ~GpuCore();

    GpuCore(const GpuCore&) = delete;
    GpuCore& operator=(const GpuCore&) = delete;
    GpuCore(GpuCore&&) = delete;
    GpuCore& operator=(GpuCore&&) = delete;

    // ── Window ──────────────────────────────────────────────────────────
    [[nodiscard]] Window& window() { return window_; }

    // ── Device ──────────────────────────────────────────────────────────
    [[nodiscard]] Device& device() { return device_; }
    [[nodiscard]] const Device& device() const { return device_; }
    [[nodiscard]] VkDevice vk_device() const { return device_.handle(); }
    [[nodiscard]] VkPhysicalDevice physical_device() const { return device_.physical_device(); }
    [[nodiscard]] VkInstance instance() const { return device_.instance(); }
    [[nodiscard]] VkQueue graphics_queue() const { return device_.graphics_queue(); }
    [[nodiscard]] uint32_t graphics_family() const;

    // ── Allocator ───────────────────────────────────────────────────────
    [[nodiscard]] VmaAllocator allocator() const { return allocator_; }

    // ── Swapchain ───────────────────────────────────────────────────────
    [[nodiscard]] Swapchain& swapchain() { return swapchain_; }
    [[nodiscard]] const Swapchain& swapchain() const { return swapchain_; }
    [[nodiscard]] VkFormat swapchain_format() const { return swapchain_.format(); }
    [[nodiscard]] VkExtent2D swapchain_extent() const { return swapchain_.extent(); }
    [[nodiscard]] uint32_t image_count() const { return swapchain_.image_count(); }

    // ── Command infrastructure ──────────────────────────────────────────
    [[nodiscard]] VkCommandPool command_pool() const { return command_pool_; }
    [[nodiscard]] VkCommandBuffer command_buffer(uint32_t frame_index) const {
        return command_buffers_[frame_index];
    }

    // Async compute command pool + per-frame CB. Null when
    // `device().async_compute_supported()` is false. Passes that hint
    // async compute route through these; cross-queue sync is a timeline
    // semaphore (see `async_compute_timeline()`).
    [[nodiscard]] VkCommandPool async_compute_pool() const { return async_compute_pool_; }
    [[nodiscard]] VkCommandBuffer async_compute_cb(uint32_t frame_index) const {
        return async_compute_cbs_.empty() ? VK_NULL_HANDLE : async_compute_cbs_[frame_index];
    }

    // Timeline semaphore for cross-queue sync between graphics and async
    // compute. One per in-flight frame to avoid head-of-line blocking
    // when consecutive frames both submit compute work. Phase 3 reads
    // and increments these; Phase 1 just owns them.
    [[nodiscard]] VkSemaphore async_compute_timeline(uint32_t frame_index) const {
        return async_compute_timeline_[frame_index];
    }

    // ── Frame sync ──────────────────────────────────────────────────────
    [[nodiscard]] VkSemaphore image_available_semaphore(uint32_t frame_index) const {
        return image_available_[frame_index];
    }
    [[nodiscard]] VkFence in_flight_fence(uint32_t frame_index) const {
        return in_flight_[frame_index];
    }

    void wait_idle();

private:
    friend class Swapchain; // cleanup() access

    Window& window_;
    Device device_;
    VmaAllocator allocator_{VK_NULL_HANDLE};
    Swapchain swapchain_;

    VkCommandPool command_pool_{VK_NULL_HANDLE};
    std::vector<VkCommandBuffer> command_buffers_;
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> image_available_{};
    std::array<VkFence, MAX_FRAMES_IN_FLIGHT> in_flight_{};

    // Async compute resources (created only when Device reports support).
    VkCommandPool async_compute_pool_{VK_NULL_HANDLE};
    std::vector<VkCommandBuffer> async_compute_cbs_;
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> async_compute_timeline_{};
};

} // namespace fjell
