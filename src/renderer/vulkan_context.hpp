#pragma once

#include "renderer/buffer.hpp"
#include "renderer/descriptor.hpp"
#include "renderer/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pipeline.hpp"
#include "renderer/swapchain.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace fjell {

class Window;
class Scene;
struct RenderStats;

class VulkanContext {
public:
    explicit VulkanContext(Window& window);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&) = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;

    void draw_frame(const glm::mat4& view, const glm::mat4& projection,
                    const glm::vec3& camera_position,
                    const struct DirectionalLight& light,
                    const Scene* scene);
    void wait_idle();
    void reload_pipeline();
    void resize_viewport(uint32_t width, uint32_t height);
    void release_viewport_texture();

    [[nodiscard]] VkDevice device() const { return device_.handle(); }
    [[nodiscard]] VkPhysicalDevice physical_device() const { return device_.physical_device(); }
    [[nodiscard]] VkCommandPool command_pool() const { return command_pool_; }
    [[nodiscard]] VkQueue graphics_queue() const { return device_.graphics_queue(); }
    [[nodiscard]] VkDescriptorSetLayout material_layout() const {
        return material_set_layout_->handle();
    }
    [[nodiscard]] VkDescriptorPool material_pool() const { return material_pool_; }
    [[nodiscard]] VkInstance instance() const { return device_.instance(); }
    [[nodiscard]] VkRenderPass render_pass() const { return swapchain_.render_pass(); }
    [[nodiscard]] uint32_t image_count() const { return swapchain_.image_count(); }
    [[nodiscard]] uint32_t graphics_family() const;
    [[nodiscard]] const std::string& gpu_name() const { return device_.gpu_name(); }
    [[nodiscard]] RenderStats& render_stats() { return *render_stats_; }
    [[nodiscard]] VkDescriptorSet viewport_texture() const { return offscreen_->imgui_texture(); }

    using OverlayCallback = std::function<void(VkCommandBuffer)>;
    void set_overlay_callback(OverlayCallback callback) { overlay_callback_ = std::move(callback); }

private:
    void create_descriptor_resources();
    void create_command_pool();
    void create_command_buffers();
    void create_sync_objects();

    void record_command_buffer(VkCommandBuffer cmd, uint32_t image_index,
                               const Scene* scene);
    void update_uniform_buffer(uint32_t frame_index,
                               const glm::mat4& view, const glm::mat4& projection,
                               const glm::vec3& camera_position,
                               const struct DirectionalLight& light);

    Window& window_;
    Device device_;
    Swapchain swapchain_;

    VkCommandPool command_pool_{VK_NULL_HANDLE};

    // Descriptors
    std::unique_ptr<DescriptorSetLayout> global_set_layout_;
    std::unique_ptr<DescriptorPool> global_pool_;
    std::array<VkDescriptorSet, 2> global_descriptor_sets_{};
    std::array<std::unique_ptr<Buffer>, 2> uniform_buffers_;

    std::unique_ptr<DescriptorSetLayout> material_set_layout_;
    VkDescriptorPool material_pool_{VK_NULL_HANDLE};

    // Offscreen scene rendering
    std::unique_ptr<OffscreenPass> offscreen_;

    // Pipeline (uses offscreen render pass)
    std::unique_ptr<Pipeline> pipeline_;

    // Command buffers and sync
    std::vector<VkCommandBuffer> command_buffers_;

    OverlayCallback overlay_callback_;

    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> image_available_{};
    std::array<VkFence, MAX_FRAMES_IN_FLIGHT> in_flight_{};
    uint32_t current_frame_{0};

    std::unique_ptr<RenderStats> render_stats_;
};

} // namespace fjell
