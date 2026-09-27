#pragma once

#include "renderer/gpu/frames_in_flight.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <functional>
#include <memory>
#include <string>

namespace fjell {

class GpuCore;
class ImGuiLayer;
struct ImGuiLayerFiles;
class Swapchain;
class Window;

/// A second OS window on the editor's GPU device, for a window that lives
/// outside the editor's own (the file browser, the import dialog): its own
/// surface, swapchain and ImGui context, drawn a frame at a time by its
/// owner. Created open; destroying it closes it.
class StandaloneWindow {
public:
    /// `files` are where its ImGui layer finds the fonts and the sRGB
    /// fragment stage, as for the layer itself.
    StandaloneWindow(GpuCore& gpu, const std::string& title, int width, int height,
                     const ImGuiLayerFiles& files);
    ~StandaloneWindow();
    StandaloneWindow(const StandaloneWindow&) = delete;
    StandaloneWindow& operator=(const StandaloneWindow&) = delete;
    StandaloneWindow(StandaloneWindow&&) = delete;
    StandaloneWindow& operator=(StandaloneWindow&&) = delete;

    /// Asks it to close, as its close button does: close_requested() then
    /// says so.
    void request_close();

    /// The user asked to close it (its title bar's close button).
    [[nodiscard]] bool close_requested() const;

    /// Draws one frame: `draw` runs in this window's ImGui context with the
    /// window's size in pixels. A frame is skipped, without calling `draw`,
    /// while the swapchain is being rebuilt.
    void frame(const std::function<void(float width, float height)>& draw);

private:
    GpuCore& gpu_;
    std::unique_ptr<Window> window_;
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    std::unique_ptr<Swapchain> swapchain_;
    std::unique_ptr<ImGuiLayer> imgui_;
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    std::array<VkCommandBuffer, MAX_FRAMES_IN_FLIGHT> command_buffers_{};
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> image_available_{};
    std::array<VkFence, MAX_FRAMES_IN_FLIGHT> in_flight_{};
    uint32_t frame_index_{0};
};

} // namespace fjell
