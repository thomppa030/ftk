#pragma once

#include "gpu/device.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/upload_context.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

namespace fjell {

class Window;

/// Core Vulkan infrastructure: device, allocator, upload lane, a command
/// pool for one-shot work, and the GPU interface's device over them. Owns the
/// destruction order, so everything VMA made is freed before the allocator.
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

    // ── Command infrastructure ──────────────────────────────────────────
    /// For one-shot command buffers on the graphics queue.
    [[nodiscard]] VkCommandPool command_pool() const { return command_pool_; }

    // ── Batched GPU uploads ─────────────────────────────────────────────
    [[nodiscard]] UploadContext& upload_context() { return *upload_; }

    // ── The GPU interface ───────────────────────────────────────────────
    /// The device of the GPU interface (gpu/device.hpp), over this core.
    /// Everything made through it must be released before the core goes.
    [[nodiscard]] gpu::Device& gpu_device() { return *gpu_device_; }
    [[nodiscard]] const gpu::Device& gpu_device() const { return *gpu_device_; }

    void wait_idle();

private:
    Window& window_;
    Device device_;
    VmaAllocator allocator_{VK_NULL_HANDLE};

    std::unique_ptr<UploadContext> upload_;
    std::unique_ptr<gpu::Device> gpu_device_;

    VkCommandPool command_pool_{VK_NULL_HANDLE};
};

} // namespace fjell
