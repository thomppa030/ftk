#include "renderer/gpu/gpu_core.hpp"
#include "gpu/vulkan/device_impl.hpp"
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

    upload_ = std::make_unique<UploadContext>(device_, allocator_);
    gpu_device_ = gpu::vulkan::create_device(*this);

    FJELL_GFX_INFO("GpuCore initialized");
}

GpuCore::~GpuCore() {
    auto dev = device_.handle();

    // Retired resources may reference the allocator and device — destroy
    // them while both are still alive, once the GPU can't touch them.
    vkDeviceWaitIdle(dev);
    // Uploads still open may clear textures the device holds: they run, and
    // finish, before the device destroys what it still has, what was
    // released first, since freeing a bind group's set needs its pools.
    upload_->wait_all();
    gpu_device_.reset();
    upload_.reset();

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (image_available_[i] != VK_NULL_HANDLE)
            vkDestroySemaphore(dev, image_available_[i], nullptr);
        if (in_flight_[i] != VK_NULL_HANDLE)
            vkDestroyFence(dev, in_flight_[i], nullptr);
    }

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
