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
    [[nodiscard]] const std::string& gpu_name() const { return gpu_name_; }

    [[nodiscard]] QueueFamilyIndices find_queue_families() const;
    [[nodiscard]] SwapchainSupport query_swapchain_support() const;
    [[nodiscard]] VkFormat find_depth_format() const;
    [[nodiscard]] VkSampleCountFlagBits max_msaa_samples() const;
    [[nodiscard]] bool mesh_shader_supported() const { return mesh_shader_supported_; }
    [[nodiscard]] uint32_t mesh_shader_max_workgroup_size() const { return mesh_shader_max_workgroup_size_; }
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
    [[nodiscard]] SwapchainSupport query_swapchain_support(VkPhysicalDevice device) const;

    Window& window_;

    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT debug_messenger_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue graphics_queue_{VK_NULL_HANDLE};
    VkQueue present_queue_{VK_NULL_HANDLE};
    std::string gpu_name_;
    bool mesh_shader_supported_{false};
    uint32_t mesh_shader_max_workgroup_size_{0};

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
