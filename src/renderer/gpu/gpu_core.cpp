#include "renderer/gpu/gpu_core.hpp"
#include "gpu/vulkan/device_impl.hpp"
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
    , allocator_{create_vma_allocator(device_)} {

    VkCommandPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_ci.queueFamilyIndex = device_.find_queue_families().graphics.value();
    if (vkCreateCommandPool(device_.handle(), &pool_ci, nullptr, &command_pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool");
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

    if (command_pool_ != VK_NULL_HANDLE)
        vkDestroyCommandPool(dev, command_pool_, nullptr);

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
