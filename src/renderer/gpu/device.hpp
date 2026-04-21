#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace fjell {

class Window;

struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;
    // Async-capable compute queue family. Prefer a dedicated family
    // (COMPUTE bit set, GRAPHICS bit clear) so work on it actually runs
    // in parallel with graphics. Left empty when no such family exists —
    // passes that opt into async degrade gracefully to the graphics queue.
    std::optional<uint32_t> async_compute;

    [[nodiscard]] bool is_complete() const {
        return graphics.has_value() && present.has_value();
    }
};

struct SwapchainSupport {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> present_modes;
};

class Device {
public:
    explicit Device(Window& window);
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    [[nodiscard]] VkInstance instance() const { return instance_; }
    [[nodiscard]] VkDevice handle() const { return device_; }
    [[nodiscard]] VkPhysicalDevice physical_device() const { return physical_device_; }
    [[nodiscard]] VkSurfaceKHR surface() const { return surface_; }
    [[nodiscard]] VkQueue graphics_queue() const { return graphics_queue_; }
    [[nodiscard]] VkQueue present_queue() const { return present_queue_; }
    [[nodiscard]] VkQueue async_compute_queue() const { return async_compute_queue_; }
    [[nodiscard]] bool async_compute_supported() const {
        return async_compute_queue_ != VK_NULL_HANDLE;
    }

    /// When async compute is supported, returns a pointer to a stable
    /// array of {graphics_family, async_compute_family} and writes the
    /// count to `out_count`. Returns nullptr (and writes 0) otherwise.
    /// The caller writes these into `VkImageCreateInfo::sharingMode =
    /// VK_SHARING_MODE_CONCURRENT` and
    /// `VkImageCreateInfo::pQueueFamilyIndices` for images that may be
    /// read or written from either queue. The array lifetime is tied to
    /// the Device instance — safe to pass into vkCreateImage.
    [[nodiscard]] const uint32_t* concurrent_queue_families(uint32_t& out_count) const;
    [[nodiscard]] const std::string& gpu_name() const { return gpu_name_; }

    [[nodiscard]] QueueFamilyIndices find_queue_families() const;
    [[nodiscard]] SwapchainSupport query_swapchain_support() const;
    [[nodiscard]] SwapchainSupport query_swapchain_support(VkSurfaceKHR surface) const;
    [[nodiscard]] VkFormat find_depth_format() const;
    [[nodiscard]] VkSampleCountFlagBits max_msaa_samples() const;
    [[nodiscard]] bool mesh_shader_supported() const { return mesh_shader_supported_; }
    [[nodiscard]] uint32_t mesh_shader_max_workgroup_size() const { return mesh_shader_max_workgroup_size_; }
    [[nodiscard]] PFN_vkCmdDrawMeshTasksEXT draw_mesh_tasks_fn() const { return pfn_draw_mesh_tasks_; }
    [[nodiscard]] PFN_vkCmdDrawMeshTasksIndirectEXT draw_mesh_tasks_indirect_fn() const {
        return pfn_draw_mesh_tasks_indirect_;
    }
    [[nodiscard]] PFN_vkCmdDrawMeshTasksIndirectCountEXT draw_mesh_tasks_indirect_count_fn() const {
        return pfn_draw_mesh_tasks_indirect_count_;
    }

    // Ray tracing
    [[nodiscard]] bool ray_tracing_supported() const { return ray_tracing_supported_; }
    [[nodiscard]] PFN_vkCreateRayTracingPipelinesKHR create_rt_pipelines_fn() const { return pfn_create_rt_pipelines_; }
    [[nodiscard]] PFN_vkCmdTraceRaysKHR cmd_trace_rays_fn() const { return pfn_cmd_trace_rays_; }
    [[nodiscard]] PFN_vkGetRayTracingShaderGroupHandlesKHR get_rt_shader_group_handles_fn() const { return pfn_get_rt_shader_group_handles_; }
    [[nodiscard]] PFN_vkCreateAccelerationStructureKHR create_accel_struct_fn() const { return pfn_create_accel_struct_; }
    [[nodiscard]] PFN_vkDestroyAccelerationStructureKHR destroy_accel_struct_fn() const { return pfn_destroy_accel_struct_; }
    [[nodiscard]] PFN_vkGetAccelerationStructureBuildSizesKHR get_accel_struct_build_sizes_fn() const { return pfn_get_accel_struct_build_sizes_; }
    [[nodiscard]] PFN_vkCmdBuildAccelerationStructuresKHR cmd_build_accel_structs_fn() const { return pfn_cmd_build_accel_structs_; }
    [[nodiscard]] PFN_vkGetAccelerationStructureDeviceAddressKHR get_accel_struct_device_address_fn() const { return pfn_get_accel_struct_device_address_; }
    [[nodiscard]] PFN_vkGetBufferDeviceAddressKHR get_buffer_device_address_fn() const { return pfn_get_buffer_device_address_; }
    [[nodiscard]] VkFormat find_supported_format(
        const std::vector<VkFormat>& candidates, VkImageTiling tiling,
        VkFormatFeatureFlags features) const;

private:
    void create_instance();
    void setup_debug_messenger();
    void create_surface();
    void pick_physical_device();
    void create_logical_device();

    [[nodiscard]] bool check_validation_layer_support() const;
    [[nodiscard]] std::vector<const char*> get_required_extensions() const;
    [[nodiscard]] QueueFamilyIndices find_queue_families(VkPhysicalDevice device) const;
    [[nodiscard]] bool is_device_suitable(VkPhysicalDevice device) const;
    [[nodiscard]] bool check_device_extension_support(VkPhysicalDevice device) const;
    [[nodiscard]] SwapchainSupport query_swapchain_support(VkPhysicalDevice device, VkSurfaceKHR surface) const;

    Window& window_;

    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT debug_messenger_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue graphics_queue_{VK_NULL_HANDLE};
    VkQueue present_queue_{VK_NULL_HANDLE};
    VkQueue async_compute_queue_{VK_NULL_HANDLE};
    // {graphics_family, async_compute_family} — stable for the lifetime
    // of the Device, filled in create_logical_device when async is
    // supported. Empty otherwise.
    std::array<uint32_t, 2> concurrent_families_{};
    uint32_t concurrent_family_count_{0};
    std::string gpu_name_;
    bool mesh_shader_supported_{false};
    uint32_t mesh_shader_max_workgroup_size_{0};
    PFN_vkCmdDrawMeshTasksEXT pfn_draw_mesh_tasks_{nullptr};
    PFN_vkCmdDrawMeshTasksIndirectEXT pfn_draw_mesh_tasks_indirect_{nullptr};
    PFN_vkCmdDrawMeshTasksIndirectCountEXT pfn_draw_mesh_tasks_indirect_count_{nullptr};

    bool ray_tracing_supported_{false};
    PFN_vkCreateRayTracingPipelinesKHR pfn_create_rt_pipelines_{nullptr};
    PFN_vkCmdTraceRaysKHR pfn_cmd_trace_rays_{nullptr};
    PFN_vkGetRayTracingShaderGroupHandlesKHR pfn_get_rt_shader_group_handles_{nullptr};
    PFN_vkCreateAccelerationStructureKHR pfn_create_accel_struct_{nullptr};
    PFN_vkDestroyAccelerationStructureKHR pfn_destroy_accel_struct_{nullptr};
    PFN_vkGetAccelerationStructureBuildSizesKHR pfn_get_accel_struct_build_sizes_{nullptr};
    PFN_vkCmdBuildAccelerationStructuresKHR pfn_cmd_build_accel_structs_{nullptr};
    PFN_vkGetAccelerationStructureDeviceAddressKHR pfn_get_accel_struct_device_address_{nullptr};
    PFN_vkGetBufferDeviceAddressKHR pfn_get_buffer_device_address_{nullptr};

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

} // namespace fjell
