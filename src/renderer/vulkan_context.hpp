#pragma once

#include "renderer/buffer.hpp"
#include "renderer/descriptor.hpp"
#include "renderer/mesh.hpp"
#include "renderer/model.hpp"
#include "renderer/texture.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
    explicit VulkanContext(Window& window);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&) = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;

    void draw_frame(const glm::mat4& view, const glm::mat4& projection);
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
    void create_depth_resources();
    void create_descriptor_resources();
    void create_graphics_pipeline();
    void create_framebuffers();
    void create_command_pool();
    void create_command_buffers();
    void create_sync_objects();
    void create_mesh();
    void load_model();

    void cleanup_swapchain();
    void recreate_swapchain();

    void record_command_buffer(VkCommandBuffer cmd, uint32_t image_index);
    void update_uniform_buffer(uint32_t frame_index,
                               const glm::mat4& view, const glm::mat4& projection);

    [[nodiscard]] static std::vector<char> read_shader(const std::string& path);
    [[nodiscard]] VkShaderModule create_shader_module(const std::vector<char>& code) const;

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
    [[nodiscard]] VkFormat find_depth_format() const;
    [[nodiscard]] VkFormat find_supported_format(
        const std::vector<VkFormat>& candidates, VkImageTiling tiling,
        VkFormatFeatureFlags features) const;

    Window& window_;

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

    VkImage depth_image_{VK_NULL_HANDLE};
    VkDeviceMemory depth_memory_{VK_NULL_HANDLE};
    VkImageView depth_image_view_{VK_NULL_HANDLE};

    VkRenderPass render_pass_{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline graphics_pipeline_{VK_NULL_HANDLE};
    std::vector<VkFramebuffer> framebuffers_;

    std::unique_ptr<DescriptorSetLayout> descriptor_set_layout_;
    std::unique_ptr<DescriptorPool> descriptor_pool_;
    std::array<VkDescriptorSet, 2> descriptor_sets_{};
    std::array<std::unique_ptr<Buffer>, 2> uniform_buffers_;

    VkCommandPool command_pool_{VK_NULL_HANDLE};
    std::vector<VkCommandBuffer> command_buffers_;

    std::unique_ptr<Mesh> mesh_;
    std::unique_ptr<Model> model_;
    std::unique_ptr<Texture> texture_;

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
