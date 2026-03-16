#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace fjell {

class Device;
class Window;

class Swapchain {
public:
    Swapchain(Device& device, Window& window);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
    Swapchain(Swapchain&&) = delete;
    Swapchain& operator=(Swapchain&&) = delete;

    void recreate();

    [[nodiscard]] VkSwapchainKHR handle() const { return swapchain_; }
    [[nodiscard]] VkRenderPass render_pass() const { return render_pass_; }
    [[nodiscard]] VkFormat format() const { return format_; }
    [[nodiscard]] VkExtent2D extent() const { return extent_; }
    [[nodiscard]] const std::vector<VkFramebuffer>& framebuffers() const { return framebuffers_; }
    [[nodiscard]] uint32_t image_count() const { return static_cast<uint32_t>(images_.size()); }
    [[nodiscard]] const std::vector<VkSemaphore>& render_finished_semaphores() const { return render_finished_; }

private:
    void create_swapchain();
    void create_image_views();
    void create_render_pass();
    void create_depth_resources();
    void create_framebuffers();
    void create_render_finished_semaphores();
    void cleanup();

    [[nodiscard]] VkSurfaceFormatKHR choose_surface_format(
        const std::vector<VkSurfaceFormatKHR>& formats) const;
    [[nodiscard]] VkPresentModeKHR choose_present_mode(
        const std::vector<VkPresentModeKHR>& modes) const;
    [[nodiscard]] VkExtent2D choose_extent(
        const VkSurfaceCapabilitiesKHR& capabilities) const;

    Device& device_;
    Window& window_;

    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
    VkFormat format_{};
    VkExtent2D extent_{};

    VkImage depth_image_{VK_NULL_HANDLE};
    VkDeviceMemory depth_memory_{VK_NULL_HANDLE};
    VkImageView depth_image_view_{VK_NULL_HANDLE};

    VkRenderPass render_pass_{VK_NULL_HANDLE};
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> render_finished_;
};

} // namespace fjell
