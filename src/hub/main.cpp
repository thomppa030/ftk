#include "hub/hub_ui.hpp"
#include "core/log.hpp"
#include "core/window.hpp"
#include "renderer/device.hpp"
#include "renderer/swapchain.hpp"
#include "ui/imgui_layer.hpp"

#include <vulkan/vulkan.h>

#include "platform/platform.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>

static constexpr int HUB_WIDTH = 480;
static constexpr int HUB_HEIGHT = 760;

static void launch_editor(const std::string& project_path) {
    auto exe = fjell::platform::executable_path();
    auto editor_path = exe.parent_path() / "fjell";
    fjell::platform::spawn_detached({editor_path.string(), project_path});
}

static void transition_image(VkCommandBuffer cmd, VkImage image,
                              VkImageLayout old_layout, VkImageLayout new_layout,
                              VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                              VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &dep);
}

int main() {
    fjell::log::init();

    try {
        fjell::Window window{"Fjell", HUB_WIDTH, HUB_HEIGHT};
        fjell::Device device{window};

        fjell::Swapchain swapchain{device, window};

        // Resolve font dir from executable location
        std::string font_dir;
        {
            auto exe_dir = fjell::platform::executable_path().parent_path();
            for (int i = 0; i < 3; ++i) {
                if (std::filesystem::is_directory(exe_dir / "engine_assets" / "fonts")) {
                    font_dir = (exe_dir / "engine_assets" / "fonts").string();
                    break;
                }
                exe_dir = exe_dir.parent_path();
            }
        }

        auto imgui_layer = fjell::ImGuiLayer(
            window.handle(), device.instance(),
            device.physical_device(), device.handle(),
            device.find_queue_families().graphics.value(),
            device.graphics_queue(),
            swapchain.format(), swapchain.image_count(),
            font_dir);

        // Command pool + buffers
        VkCommandPool cmd_pool;
        {
            VkCommandPoolCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            ci.queueFamilyIndex = device.find_queue_families().graphics.value();
            vkCreateCommandPool(device.handle(), &ci, nullptr, &cmd_pool);
        }

        constexpr uint32_t MAX_FRAMES = 2;
        std::array<VkCommandBuffer, MAX_FRAMES> cmd_bufs{};
        {
            VkCommandBufferAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            ai.commandPool = cmd_pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = MAX_FRAMES;
            vkAllocateCommandBuffers(device.handle(), &ai, cmd_bufs.data());
        }

        std::array<VkFence, MAX_FRAMES> fences{};
        std::array<VkSemaphore, MAX_FRAMES> image_ready{};
        for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
            VkFenceCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(device.handle(), &fi, nullptr, &fences[i]);

            VkSemaphoreCreateInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            vkCreateSemaphore(device.handle(), &si, nullptr, &image_ready[i]);
        }
        auto& render_done = swapchain.render_finished_semaphores();

        fjell::hub::HubUI hub_ui;
        uint32_t frame = 0;
        float last_time = static_cast<float>(glfwGetTime());

        while (!window.should_close()) {
            window.poll_events();

            float now = static_cast<float>(glfwGetTime());
            float dt = now - last_time;
            last_time = now;
            hub_ui.update_splash(dt);

            vkWaitForFences(device.handle(), 1, &fences[frame], VK_TRUE, UINT64_MAX);
            vkResetFences(device.handle(), 1, &fences[frame]);

            uint32_t img_idx = 0;
            auto result = vkAcquireNextImageKHR(
                device.handle(), swapchain.handle(), UINT64_MAX,
                image_ready[frame], VK_NULL_HANDLE, &img_idx);
            if (result == VK_ERROR_OUT_OF_DATE_KHR) {
                swapchain.recreate();
                continue;
            }

            vkResetCommandBuffer(cmd_bufs[frame], 0);
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(cmd_bufs[frame], &begin);

            // Transition swapchain image: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL
            transition_image(cmd_bufs[frame], swapchain.image(img_idx),
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            VkRenderingAttachmentInfo color_attachment{};
            color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color_attachment.imageView = swapchain.image_view(img_idx);
            color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color_attachment.clearValue.color = {{0.012f, 0.012f, 0.015f, 1.0f}};

            VkRenderingInfo rendering_info{};
            rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering_info.renderArea.extent = swapchain.extent();
            rendering_info.layerCount = 1;
            rendering_info.colorAttachmentCount = 1;
            rendering_info.pColorAttachments = &color_attachment;

            vkCmdBeginRendering(cmd_bufs[frame], &rendering_info);

            imgui_layer.begin_frame();
            auto launch = hub_ui.draw(
                static_cast<float>(swapchain.extent().width),
                static_cast<float>(swapchain.extent().height));
            imgui_layer.end_frame();
            imgui_layer.render(cmd_bufs[frame]);

            vkCmdEndRendering(cmd_bufs[frame]);

            // Transition swapchain image: COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR
            transition_image(cmd_bufs[frame], swapchain.image(img_idx),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

            vkEndCommandBuffer(cmd_bufs[frame]);

            VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &image_ready[frame];
            submit.pWaitDstStageMask = &wait_stage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd_bufs[frame];
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &render_done[img_idx];

            vkQueueSubmit(device.graphics_queue(), 1, &submit, fences[frame]);

            VkSwapchainKHR sc = swapchain.handle();
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &render_done[img_idx];
            present.swapchainCount = 1;
            present.pSwapchains = &sc;
            present.pImageIndices = &img_idx;
            vkQueuePresentKHR(device.graphics_queue(), &present);

            frame = (frame + 1) % MAX_FRAMES;

            if (!launch.empty()) {
                launch_editor(launch);
                glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
            }
        }

        vkDeviceWaitIdle(device.handle());

        for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
            vkDestroySemaphore(device.handle(), image_ready[i], nullptr);
            vkDestroyFence(device.handle(), fences[i], nullptr);
        }
        vkDestroyCommandPool(device.handle(), cmd_pool, nullptr);

    } catch (const std::exception& e) {
        FJELL_CORE_CRITICAL("Fatal: {}", e.what());
        fjell::log::shutdown();
        return EXIT_FAILURE;
    }

    fjell::log::shutdown();
    return EXIT_SUCCESS;
}
