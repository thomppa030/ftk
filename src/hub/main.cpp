#include "hub/hub_ui.hpp"
#include "core/log.hpp"
#include "core/window.hpp"
#include "renderer/gpu/gpu_core.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "ui/imgui_layer.hpp"

#include <vulkan/vulkan.h>

#include "platform/platform.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

static constexpr int HUB_WIDTH = 480;
static constexpr int HUB_HEIGHT = 760;

static void launch_editor(const std::string& project_path) {
    auto exe = fjell::platform::executable_path();
    auto editor_path = exe.parent_path() / "fjell";
    auto result = fjell::platform::exec_replace({editor_path.string(), project_path});
    // exec_replace only returns on failure
    FJELL_CORE_ERROR("Failed to launch editor: {}", result.error());
}

static std::string resolve_font_dir() {
    auto exe_dir = fjell::platform::executable_path().parent_path();
    for (int i = 0; i < 3; ++i) {
        if (std::filesystem::is_directory(exe_dir / "engine_assets" / "fonts")) {
            return (exe_dir / "engine_assets" / "fonts").string();
        }
        exe_dir = exe_dir.parent_path();
    }
    return {};
}

int main() {
    fjell::log::init();
    std::string pending_launch;

    try {
        fjell::Window window{"Fjell", HUB_WIDTH, HUB_HEIGHT};
        fjell::GpuCore gpu{window};

        auto imgui_layer = fjell::ImGuiLayer(
            window.handle(), gpu.instance(),
            gpu.physical_device(), gpu.vk_device(),
            gpu.graphics_family(), gpu.graphics_queue(),
            gpu.swapchain_format(), gpu.image_count(),
            resolve_font_dir());

        const auto& render_done = gpu.swapchain().render_finished_semaphores();

        fjell::hub::HubUI hub_ui;
        uint32_t frame = 0;
        float last_time = static_cast<float>(glfwGetTime());

        while (!window.should_close()) {
            window.poll_events();

            float now = static_cast<float>(glfwGetTime());
            float dt = now - last_time;
            last_time = now;
            hub_ui.update_splash(dt);

            VkFence fence = gpu.in_flight_fence(frame);
            vkWaitForFences(gpu.vk_device(), 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(gpu.vk_device(), 1, &fence);

            uint32_t img_idx = 0;
            auto result = vkAcquireNextImageKHR(
                gpu.vk_device(), gpu.swapchain().handle(), UINT64_MAX,
                gpu.image_available_semaphore(frame), VK_NULL_HANDLE, &img_idx);
            if (result == VK_ERROR_OUT_OF_DATE_KHR) {
                gpu.swapchain().recreate();
                continue;
            }

            auto cmd = gpu.command_buffer(frame);
            vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(cmd, &begin);

            fjell::vk_utils::prepare_color_attachment(cmd, gpu.swapchain().image(img_idx));

            VkRenderingAttachmentInfo color_attachment{};
            color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color_attachment.imageView = gpu.swapchain().image_view(img_idx);
            color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color_attachment.clearValue.color = {{0.012f, 0.012f, 0.015f, 1.0f}};

            VkRenderingInfo rendering_info{};
            rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering_info.renderArea.extent = gpu.swapchain_extent();
            rendering_info.layerCount = 1;
            rendering_info.colorAttachmentCount = 1;
            rendering_info.pColorAttachments = &color_attachment;

            vkCmdBeginRendering(cmd, &rendering_info);

            imgui_layer.begin_frame();
            auto launch = hub_ui.draw(
                static_cast<float>(gpu.swapchain_extent().width),
                static_cast<float>(gpu.swapchain_extent().height));
            imgui_layer.end_frame();
            imgui_layer.render(cmd);

            vkCmdEndRendering(cmd);

            fjell::vk_utils::transition_image(cmd, gpu.swapchain().image(img_idx),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

            vkEndCommandBuffer(cmd);

            VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSemaphore wait_sem = gpu.image_available_semaphore(frame);
            VkSemaphore signal_sem = render_done[img_idx];

            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &wait_sem;
            submit.pWaitDstStageMask = &wait_stage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &signal_sem;

            vkQueueSubmit(gpu.graphics_queue(), 1, &submit, fence);

            VkSwapchainKHR sc = gpu.swapchain().handle();
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &signal_sem;
            present.swapchainCount = 1;
            present.pSwapchains = &sc;
            present.pImageIndices = &img_idx;
            vkQueuePresentKHR(gpu.graphics_queue(), &present);

            frame = (frame + 1) % fjell::MAX_FRAMES_IN_FLIGHT;

            if (!launch.empty()) {
                pending_launch = launch;
                glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
            }
        }

        gpu.wait_idle();

    } catch (const std::exception& e) {
        FJELL_CORE_CRITICAL("Fatal: {}", e.what());
        fjell::log::shutdown();
        return EXIT_FAILURE;
    }

    fjell::log::shutdown();

    // exec into the editor after all cleanup — replaces this process,
    // so the shell waits for the editor naturally and logs go to the terminal.
    if (!pending_launch.empty()) {
        launch_editor(pending_launch);
    }

    return EXIT_SUCCESS;
}
