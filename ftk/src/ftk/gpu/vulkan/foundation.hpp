#pragma once

#include "ftk/base/result.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ftk {
class Window;
}

namespace ftk::gpu::vulkan {

class UploadLanes;

struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;
    // Async-capable compute queue family. Prefer a dedicated family
    // (COMPUTE bit set, GRAPHICS bit clear) so work on it actually runs
    // in parallel with graphics. Left empty when no such family exists —
    // passes that opt into async degrade gracefully to the graphics queue.
    std::optional<uint32_t> async_compute;
    // Dedicated transfer family (TRANSFER bit set, GRAPHICS and COMPUTE
    // clear) — the DMA engine. Copies submitted here run in parallel with
    // rendering. Left empty when no such family exists — uploads degrade
    // gracefully to the graphics queue.
    std::optional<uint32_t> transfer;

    [[nodiscard]] bool is_complete() const {
        return graphics.has_value() && present.has_value();
    }
};

struct SwapchainSupport {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> present_modes;
};

/// What a device runs on, brought up before anything it makes and torn down
/// after it: the Vulkan instance, the GPU chosen and its queues, the
/// extensions loaded from it, the allocator, and the lanes uploads run on.
class Foundation {
public:
    /// Brings everything up for a GPU that can show `window`, for the program
    /// named `program`. Throws std::runtime_error when there is none, or the
    /// driver refuses.
    Foundation(const Window& window, const std::string& program);
    ~Foundation();

    Foundation(const Foundation&) = delete;
    Foundation& operator=(const Foundation&) = delete;
    Foundation(Foundation&&) = delete;
    Foundation& operator=(Foundation&&) = delete;

    /// A surface for `window` to present to, which the caller destroys.
    /// @return the surface, or why the platform could not make one
    [[nodiscard]] Result<VkSurfaceKHR> create_surface(const Window& window) const;

    [[nodiscard]] VkInstance instance() const { return instance_; }
    [[nodiscard]] VkDevice handle() const { return device_; }
    [[nodiscard]] VkPhysicalDevice physical_device() const { return physical_device_; }
    [[nodiscard]] VkQueue graphics_queue() const { return graphics_queue_; }
    [[nodiscard]] VkQueue present_queue() const { return present_queue_; }
    [[nodiscard]] VkQueue async_compute_queue() const { return async_compute_queue_; }
    [[nodiscard]] bool async_compute_supported() const {
        return async_compute_queue_ != VK_NULL_HANDLE;
    }
    [[nodiscard]] VkQueue transfer_queue() const { return transfer_queue_; }
    [[nodiscard]] bool transfer_queue_supported() const {
        return transfer_queue_ != VK_NULL_HANDLE;
    }

    /// Sparse binding lets a buffer reserve a large virtual range and bind
    /// memory pages on demand — it can grow in place without ever changing
    /// its handle or device address. sparse_bind_queue() is the queue to
    /// submit vkQueueBindSparse on (the dedicated transfer queue when its
    /// family supports sparse ops, so binds never touch the graphics queue).
    [[nodiscard]] VkQueue sparse_bind_queue() const { return sparse_bind_queue_; }

    /// Queue families that may access upload-destination buffers: graphics,
    /// plus the dedicated transfer and async compute families when they
    /// exist. With two or more entries, buffers written by the upload path
    /// must be created VK_SHARING_MODE_CONCURRENT over these families so
    /// their content stays defined across queues without ownership
    /// transfers. The span is stable for the foundation's lifetime.
    [[nodiscard]] std::span<const uint32_t> upload_sharing_families() const {
        return {upload_families_.data(), upload_family_count_};
    }

    /// When async compute is supported, returns a pointer to a stable
    /// array of {graphics_family, async_compute_family} and writes the
    /// count to `out_count`. Returns nullptr (and writes 0) otherwise.
    /// The caller writes these into `VkImageCreateInfo::sharingMode =
    /// VK_SHARING_MODE_CONCURRENT` and
    /// `VkImageCreateInfo::pQueueFamilyIndices` for images that may be
    /// read or written from either queue. The array lives as long as the
    /// foundation — safe to pass into vkCreateImage.
    [[nodiscard]] const uint32_t* concurrent_queue_families(uint32_t& out_count) const;
    [[nodiscard]] const std::string& gpu_name() const { return gpu_name_; }

    /// The queue families chosen: graphics, present (for the window the GPU
    /// was chosen to show), and the dedicated ones where there are.
    [[nodiscard]] const QueueFamilyIndices& queue_families() const { return families_; }
    [[nodiscard]] SwapchainSupport query_swapchain_support(VkSurfaceKHR surface) const;
    [[nodiscard]] VkSampleCountFlagBits max_msaa_samples() const;
    [[nodiscard]] bool mesh_shader_supported() const { return mesh_shader_supported_; }
    /// Per-workgroup output ceiling a mesh shader may declare via
    /// `layout(..., max_vertices = N, max_primitives = M) out;`. Geometry
    /// generated in-shader must size its patches against these.
    [[nodiscard]] uint32_t mesh_shader_max_output_vertices() const {
        return mesh_shader_max_output_vertices_;
    }
    [[nodiscard]] uint32_t mesh_shader_max_output_primitives() const {
        return mesh_shader_max_output_primitives_;
    }
    [[nodiscard]] PFN_vkCmdDrawMeshTasksEXT draw_mesh_tasks_fn() const { return pfn_draw_mesh_tasks_; }
    [[nodiscard]] PFN_vkCmdDrawMeshTasksIndirectEXT draw_mesh_tasks_indirect_fn() const {
        return pfn_draw_mesh_tasks_indirect_;
    }
    [[nodiscard]] PFN_vkCmdDrawMeshTasksIndirectCountEXT draw_mesh_tasks_indirect_count_fn() const {
        return pfn_draw_mesh_tasks_indirect_count_;
    }

    // Ray tracing
    /// True when acceleration structures and ray queries are both enabled on
    /// this device; the ray traced passes trace from compute shaders. False
    /// leaves every ray traced pass off and DDGI on its SDF trace.
    [[nodiscard]] bool ray_tracing_supported() const { return ray_tracing_supported_; }
    [[nodiscard]] PFN_vkCreateAccelerationStructureKHR create_accel_struct_fn() const { return pfn_create_accel_struct_; }
    [[nodiscard]] PFN_vkDestroyAccelerationStructureKHR destroy_accel_struct_fn() const { return pfn_destroy_accel_struct_; }
    [[nodiscard]] PFN_vkGetAccelerationStructureBuildSizesKHR get_accel_struct_build_sizes_fn() const { return pfn_get_accel_struct_build_sizes_; }
    [[nodiscard]] PFN_vkCmdBuildAccelerationStructuresKHR cmd_build_accel_structs_fn() const { return pfn_cmd_build_accel_structs_; }
    [[nodiscard]] PFN_vkGetAccelerationStructureDeviceAddressKHR get_accel_struct_device_address_fn() const { return pfn_get_accel_struct_device_address_; }

    // Device-loss diagnostics. Call after a VK_ERROR_DEVICE_LOST; if
    // VK_EXT_device_fault is supported, queries and logs the faulting
    // address ranges + vendor info to pin the GPU op that lost the device.
    void dump_device_fault(const char* context) const;

    /// What every buffer and image the device makes is allocated from.
    [[nodiscard]] VmaAllocator allocator() const { return allocator_; }

    /// The lanes uploads run on, which frames wait for.
    [[nodiscard]] UploadLanes& lanes() { return *lanes_; }

private:
    void create_instance(const std::string& program);
    void setup_debug_messenger();
    void pick_physical_device(VkSurfaceKHR shown);
    void create_logical_device();
    void create_allocator();

    [[nodiscard]] bool check_validation_layer_support() const;
    [[nodiscard]] std::vector<const char*> get_required_extensions() const;
    [[nodiscard]] QueueFamilyIndices find_queue_families(VkPhysicalDevice device, VkSurfaceKHR shown) const;
    [[nodiscard]] bool is_device_suitable(VkPhysicalDevice device, VkSurfaceKHR shown) const;
    [[nodiscard]] bool check_device_extension_support(VkPhysicalDevice device) const;
    [[nodiscard]] SwapchainSupport query_swapchain_support(VkPhysicalDevice device, VkSurfaceKHR surface) const;

    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT debug_messenger_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    QueueFamilyIndices families_;
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue graphics_queue_{VK_NULL_HANDLE};
    VkQueue present_queue_{VK_NULL_HANDLE};
    VkQueue async_compute_queue_{VK_NULL_HANDLE};
    VkQueue transfer_queue_{VK_NULL_HANDLE};
    VkQueue sparse_bind_queue_{VK_NULL_HANDLE};
    // Families for upload-destination buffer sharing — see
    // upload_sharing_families().
    std::array<uint32_t, 3> upload_families_{};
    uint32_t upload_family_count_{0};
    // {graphics_family, async_compute_family} — stable for the lifetime
    // of the foundation, filled in create_logical_device when async is
    // supported. Empty otherwise.
    std::array<uint32_t, 2> concurrent_families_{};
    uint32_t concurrent_family_count_{0};
    std::string gpu_name_;
    bool mesh_shader_supported_{false};
    uint32_t mesh_shader_max_output_vertices_{0};
    uint32_t mesh_shader_max_output_primitives_{0};
    PFN_vkCmdDrawMeshTasksEXT pfn_draw_mesh_tasks_{nullptr};
    PFN_vkCmdDrawMeshTasksIndirectEXT pfn_draw_mesh_tasks_indirect_{nullptr};
    PFN_vkCmdDrawMeshTasksIndirectCountEXT pfn_draw_mesh_tasks_indirect_count_{nullptr};

    bool device_fault_supported_{false};

    bool ray_tracing_supported_{false};
    PFN_vkCreateAccelerationStructureKHR pfn_create_accel_struct_{nullptr};
    PFN_vkDestroyAccelerationStructureKHR pfn_destroy_accel_struct_{nullptr};
    PFN_vkGetAccelerationStructureBuildSizesKHR pfn_get_accel_struct_build_sizes_{nullptr};
    PFN_vkCmdBuildAccelerationStructuresKHR pfn_cmd_build_accel_structs_{nullptr};
    PFN_vkGetAccelerationStructureDeviceAddressKHR pfn_get_accel_struct_device_address_{nullptr};

    VmaAllocator allocator_{VK_NULL_HANDLE};
    std::unique_ptr<UploadLanes> lanes_;

#ifdef NDEBUG
    static constexpr bool enable_validation_ = false;
#else
    static constexpr bool enable_validation_ = true;
#endif

    static constexpr std::array validation_layers_ = {
        "VK_LAYER_KHRONOS_validation"
    };

    static constexpr std::array required_device_extensions_ = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    std::vector<const char*> device_extensions_;
};

} // namespace ftk::gpu::vulkan
