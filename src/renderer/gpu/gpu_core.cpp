#include "renderer/gpu/gpu_core.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu/window.hpp"
#include "core/log.hpp"

#include <stdexcept>

namespace fjell {

static VmaAllocator create_vma_allocator(const Device& device) {
    VmaAllocatorCreateInfo vma_ci{};
    vma_ci.instance = device.instance();
    vma_ci.physicalDevice = device.physical_device();
    vma_ci.device = device.handle();
    vma_ci.vulkanApiVersion = VK_API_VERSION_1_3;
    if (device.ray_tracing_supported()) {
        vma_ci.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }
    VmaAllocator allocator{VK_NULL_HANDLE};
    if (vmaCreateAllocator(&vma_ci, &allocator) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create VMA allocator");
    }
    return allocator;
}

GpuCore::GpuCore(Window& window)
    : window_{window}
    , device_{window}
    , allocator_{create_vma_allocator(device_)}
    , swapchain_{device_, allocator_, window} {

    // Command pool
    auto indices = device_.find_queue_families();
    VkCommandPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_ci.queueFamilyIndex = indices.graphics.value();
    if (vkCreateCommandPool(device_.handle(), &pool_ci, nullptr, &command_pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool");
    }

    // Command buffers
    command_buffers_.resize(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    if (vkAllocateCommandBuffers(device_.handle(), &alloc_info, command_buffers_.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffers");
    }
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        set_cb_debug_name(device_.handle(), command_buffers_[i],
                          "graphics_pre[" + std::to_string(i) + "]");
    }

    // Sync objects
    VkSemaphoreCreateInfo sem_ci{};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fence_ci{};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (vkCreateSemaphore(device_.handle(), &sem_ci, nullptr, &image_available_[i]) != VK_SUCCESS ||
            vkCreateFence(device_.handle(), &fence_ci, nullptr, &in_flight_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create sync objects");
        }
    }

    // Async compute infrastructure — only when the device actually offers
    // a separate compute family. On single-family hardware we skip the
    // pool entirely; passes that hint async degrade to the graphics queue
    // in Phase 3.
    if (device_.async_compute_supported()) {
        // Post-compute graphics CBs — allocated from the existing
        // graphics command pool. Same queue family as command_buffer(),
        // just a second CB per frame slot so async-frames can split
        // graphics work into before/after halves around the compute
        // submit (phase 3d).
        graphics_post_cbs_.resize(MAX_FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo post_alloc{};
        post_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        post_alloc.commandPool = command_pool_;
        post_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        post_alloc.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
        if (vkAllocateCommandBuffers(device_.handle(), &post_alloc, graphics_post_cbs_.data()) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate post-compute graphics command buffers");
        }
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            set_cb_debug_name(device_.handle(), graphics_post_cbs_[i],
                              "graphics_post[" + std::to_string(i) + "]");
        }

        VkCommandPoolCreateInfo ac_pool_ci{};
        ac_pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ac_pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ac_pool_ci.queueFamilyIndex = indices.async_compute.value();
        if (vkCreateCommandPool(device_.handle(), &ac_pool_ci, nullptr, &async_compute_pool_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create async compute command pool");
        }

        async_compute_cbs_.resize(MAX_FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo ac_alloc{};
        ac_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ac_alloc.commandPool = async_compute_pool_;
        ac_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ac_alloc.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
        if (vkAllocateCommandBuffers(device_.handle(), &ac_alloc, async_compute_cbs_.data()) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate async compute command buffers");
        }
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            set_cb_debug_name(device_.handle(), async_compute_cbs_[i],
                              "async_compute[" + std::to_string(i) + "]");
        }

        // Timeline semaphores start at value 0. Phase 3 increments
        // monotonically per frame; comparing signaled value vs a target
        // gives us wait-for-GPU-work semantics without reset.
        VkSemaphoreTypeCreateInfo tl_ci{};
        tl_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        tl_ci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        tl_ci.initialValue = 0;

        VkSemaphoreCreateInfo tl_sem_ci{};
        tl_sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        tl_sem_ci.pNext = &tl_ci;
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            if (vkCreateSemaphore(device_.handle(), &tl_sem_ci, nullptr,
                                   &async_compute_timeline_[i]) != VK_SUCCESS) {
                throw std::runtime_error("Failed to create async compute timeline semaphore");
            }
        }
    }

    upload_ = std::make_unique<UploadContext>(device_, allocator_);

    FJELL_GFX_INFO("GpuCore initialized");
}

GpuCore::~GpuCore() {
    auto dev = device_.handle();

    // Retired resources may reference the allocator and device — destroy
    // them while both are still alive, once the GPU can't touch them.
    vkDeviceWaitIdle(dev);
    deferred_deleter_.flush_all();
    upload_.reset();

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (image_available_[i] != VK_NULL_HANDLE)
            vkDestroySemaphore(dev, image_available_[i], nullptr);
        if (in_flight_[i] != VK_NULL_HANDLE)
            vkDestroyFence(dev, in_flight_[i], nullptr);
        if (async_compute_timeline_[i] != VK_NULL_HANDLE)
            vkDestroySemaphore(dev, async_compute_timeline_[i], nullptr);
    }

    if (async_compute_pool_ != VK_NULL_HANDLE)
        vkDestroyCommandPool(dev, async_compute_pool_, nullptr);

    if (command_pool_ != VK_NULL_HANDLE)
        vkDestroyCommandPool(dev, command_pool_, nullptr);

    // Swapchain depth image is VMA-backed — must be freed before allocator.
    swapchain_.cleanup();

    if (allocator_ != VK_NULL_HANDLE)
        vmaDestroyAllocator(allocator_);
}

uint32_t GpuCore::graphics_family() const {
    return device_.find_queue_families().graphics.value();
}

void GpuCore::wait_idle() {
    vkDeviceWaitIdle(device_.handle());
}

} // namespace fjell
