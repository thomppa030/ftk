#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace fjell {

class Device;
class Window;

class Swapchain {
public:
    Swapchain(Device& device, VmaAllocator allocator, Window& window);
    /// Construct with an explicit surface (for secondary windows sharing a Device).
    Swapchain(Device& device, VmaAllocator allocator, Window& window, VkSurfaceKHR surface);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
    Swapchain(Swapchain&&) = delete;
    Swapchain& operator=(Swapchain&&) = delete;

    void recreate();

    [[nodiscard]] VkSwapchainKHR handle() const { return swapchain_; }
    [[nodiscard]] VkFormat format() const { return format_; }
    [[nodiscard]] VkExtent2D extent() const { return extent_; }
    [[nodiscard]] VkImage image(uint32_t index) const { return images_[index]; }
    [[nodiscard]] VkImageView image_view(uint32_t index) const { return image_views_[index]; }
    [[nodiscard]] VkImageView depth_view() const { return depth_image_view_; }
    [[nodiscard]] uint32_t image_count() const { return static_cast<uint32_t>(images_.size()); }
    [[nodiscard]] const std::vector<VkSemaphore>& render_finished_semaphores() const { return render_finished_; }

    /// Next acquire semaphore to hand to vkAcquireNextImageKHR. Rotates through
    /// a pool sized to image_count+1 so a skipped/failed frame can never leave
    /// a still-signaled semaphore that the next acquire would reuse (the
    /// "semaphore must not have any pending operations" desync). Owned and
    /// recreated by the swapchain so recreate() can never orphan a signal.
    [[nodiscard]] VkSemaphore next_acquire_semaphore();

    friend class GpuCore;

private:
    void create_swapchain();
    void create_image_views();
    void create_depth_resources();
    void create_render_finished_semaphores();
    void create_acquire_semaphores();
    void cleanup();

    [[nodiscard]] VkSurfaceFormatKHR choose_surface_format(
        const std::vector<VkSurfaceFormatKHR>& formats) const;
    [[nodiscard]] VkPresentModeKHR choose_present_mode(
        const std::vector<VkPresentModeKHR>& modes) const;
    [[nodiscard]] VkExtent2D choose_extent(
        const VkSurfaceCapabilitiesKHR& capabilities) const;

    [[nodiscard]] VkSurfaceKHR active_surface() const;

    Device& device_;
    VmaAllocator allocator_{VK_NULL_HANDLE};
    Window& window_;
    VkSurfaceKHR surface_override_{VK_NULL_HANDLE};

    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
    VkFormat format_{};
    VkExtent2D extent_{};

    VkImage depth_image_{VK_NULL_HANDLE};
    VmaAllocation depth_allocation_{VK_NULL_HANDLE};
    VkImageView depth_image_view_{VK_NULL_HANDLE};

    std::vector<VkSemaphore> render_finished_;

    // Acquire-semaphore pool (image_count+1), rotated by next_acquire_semaphore().
    std::vector<VkSemaphore> acquire_semaphores_;
    uint32_t acquire_index_{0};
};

} // namespace fjell
