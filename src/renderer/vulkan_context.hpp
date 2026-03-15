#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

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

class VulkanContext {
public:
    explicit VulkanContext(const Window& window);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&) = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;

    void draw_frame();
    void wait_idle();

private:
    void create_instance();
    void setup_debug_messenger();
    void create_surface();
    void pick_physical_device();
    void create_logical_device();
    void create_swapchain();
    void create_image_views();
    void create_render_pass();
    void create_framebuffers();
    void create_command_pool();
    void create_command_buffers();
    void create_sync_objects();

    void record_command_buffer(VkCommandBuffer cmd, uint32_t image_index);

    [[nodiscard]] bool check_validation_layer_support() const;
    [[nodiscard]] std::vector<const char*> get_required_extensions() const;
    [[nodiscard]] QueueFamilyIndices find_queue_families(VkPhysicalDevice device) const;
    [[nodiscard]] bool is_device_suitable(VkPhysicalDevice device) const;
    [[nodiscard]] bool check_device_extension_support(VkPhysicalDevice device) const;
    [[nodiscard]] SwapchainSupport query_swapchain_support(VkPhysicalDevice device) const;

    [[nodiscard]] VkSurfaceFormatKHR choose_surface_format(
        const std::vector<VkSurfaceFormatKHR>& formats) const;
    [[nodiscard]] VkPresentModeKHR choose_present_mode(
        const std::vector<VkPresentModeKHR>& modes) const;
    [[nodiscard]] VkExtent2D choose_extent(
        const VkSurfaceCapabilitiesKHR& capabilities) const;

    const Window& window_;

    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT debug_messenger_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue graphics_queue_{VK_NULL_HANDLE};
    VkQueue present_queue_{VK_NULL_HANDLE};

    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;
    VkFormat swapchain_format_{};
    VkExtent2D swapchain_extent_{};

    VkRenderPass render_pass_{VK_NULL_HANDLE};
    std::vector<VkFramebuffer> framebuffers_;

    VkCommandPool command_pool_{VK_NULL_HANDLE};
    std::vector<VkCommandBuffer> command_buffers_;

    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> image_available_{};
    std::array<VkFence, MAX_FRAMES_IN_FLIGHT> in_flight_{};
    std::vector<VkSemaphore> render_finished_;
    uint32_t current_frame_{0};

#ifdef NDEBUG
    static constexpr bool enable_validation_ = false;
#else
    static constexpr bool enable_validation_ = true;
#endif

    static constexpr std::array validation_layers_ = {
        "VK_LAYER_KHRONOS_validation"
    };

    static constexpr std::array device_extensions_ = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };
};

} // namespace fjell
