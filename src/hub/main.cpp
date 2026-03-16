#include "hub/hub_ui.hpp"
#include "core/log.hpp"
#include "core/window.hpp"
#include "renderer/device.hpp"
#include "renderer/swapchain.hpp"
#include "ui/imgui_layer.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdlib>
#include <string>
#include <unistd.h>

static constexpr int HUB_WIDTH = 480;
static constexpr int HUB_HEIGHT = 760;

static void launch_editor(const std::string& project_path) {
    pid_t pid = fork();
    if (pid == 0) {
        char self[4096];
        auto len = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (len <= 0) _exit(1);
        self[len] = '\0';

        std::string hub_path(self);
        auto pos = hub_path.rfind('/');
        std::string editor_path = hub_path.substr(0, pos + 1) + "fjell";

        execl(editor_path.c_str(), "fjell", project_path.c_str(), nullptr);
        _exit(1);
    }
}

int main() {
    fjell::log::init();

    try {
        fjell::Window window{"Fjell", HUB_WIDTH, HUB_HEIGHT};
        fjell::Device device{window};

        fjell::Swapchain swapchain{device, window};

        auto imgui_layer = fjell::ImGuiLayer(
            window.handle(), device.instance(),
            device.physical_device(), device.handle(),
            device.find_queue_families().graphics.value(),
            device.graphics_queue(),
            swapchain.render_pass(), swapchain.image_count());

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
        // render_finished semaphores: one per swapchain image (owned by Swapchain)
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

            std::array<VkClearValue, 2> clears{};
            clears[0].color = {{0.012f, 0.012f, 0.015f, 1.0f}}; // #1e1f22 linearized
            clears[1].depthStencil = {1.0f, 0};

            VkRenderPassBeginInfo rp{};
            rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rp.renderPass = swapchain.render_pass();
            rp.framebuffer = swapchain.framebuffers()[img_idx];
            rp.renderArea.extent = swapchain.extent();
            rp.clearValueCount = static_cast<uint32_t>(clears.size());
            rp.pClearValues = clears.data();

            vkCmdBeginRenderPass(cmd_bufs[frame], &rp, VK_SUBPASS_CONTENTS_INLINE);

            imgui_layer.begin_frame();
            auto launch = hub_ui.draw(
                static_cast<float>(swapchain.extent().width),
                static_cast<float>(swapchain.extent().height));
            imgui_layer.end_frame();
            imgui_layer.render(cmd_bufs[frame]);

            vkCmdEndRenderPass(cmd_bufs[frame]);
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
