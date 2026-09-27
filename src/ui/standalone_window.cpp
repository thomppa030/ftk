#include "ui/standalone_window.hpp"

#include "renderer/gpu/gpu_core.hpp"
#include "renderer/gpu/swapchain.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "renderer/gpu/window.hpp"
#include "ui/engine_imgui_files.hpp"
#include "ui/imgui_layer.hpp"


#include <cstdint>

namespace fjell {

namespace {

// Under the ImGui window, which covers all of it.
constexpr VkClearColorValue CLEAR{{0.012f, 0.012f, 0.015f, 1.0f}};

} // namespace

StandaloneWindow::StandaloneWindow(GpuCore& gpu, const std::string& title, int width, int height) : gpu_{gpu} {
    window_ = std::make_unique<Window>(title, width, height);
    surface_ = window_->create_surface(gpu_.instance());
    swapchain_ = std::make_unique<Swapchain>(gpu_.device(), gpu_.allocator(), *window_, surface_);

    const VkDevice dev = gpu_.vk_device();
    VkCommandPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_ci.queueFamilyIndex = gpu_.graphics_family();
    vk_check(vkCreateCommandPool(dev, &pool_ci, nullptr, &command_pool_), "standalone window command pool");

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    vk_check(vkAllocateCommandBuffers(dev, &alloc_info, command_buffers_.data()), "standalone window command buffers");

    VkSemaphoreCreateInfo sem_ci{};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fence_ci{};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        vk_check(vkCreateSemaphore(dev, &sem_ci, nullptr, &image_available_[i]), "standalone window semaphore");
        vk_check(vkCreateFence(dev, &fence_ci, nullptr, &in_flight_[i]), "standalone window fence");
    }

    imgui_ = std::make_unique<ImGuiLayer>(window_->handle(), gpu_.instance(), gpu_.physical_device(), gpu_.vk_device(),
                                          gpu_.graphics_family(), gpu_.graphics_queue(), swapchain_->format(),
                                          swapchain_->image_count(), engine_imgui_files());
}

StandaloneWindow::~StandaloneWindow() {
    const VkDevice dev = gpu_.vk_device();
    vkDeviceWaitIdle(dev);
    imgui_.reset();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (image_available_[i] != VK_NULL_HANDLE) vkDestroySemaphore(dev, image_available_[i], nullptr);
        if (in_flight_[i] != VK_NULL_HANDLE) vkDestroyFence(dev, in_flight_[i], nullptr);
    }
    if (command_pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(dev, command_pool_, nullptr);
    swapchain_.reset();
    if (surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(gpu_.instance(), surface_, nullptr);
    window_.reset();
}

void StandaloneWindow::request_close() {
    window_->request_close();
}

bool StandaloneWindow::close_requested() const {
    return window_->should_close();
}

void StandaloneWindow::frame(const std::function<void(float width, float height)>& draw) {
    const VkDevice dev = gpu_.vk_device();
    vk_check(vkWaitForFences(dev, 1, &in_flight_[frame_index_], VK_TRUE, UINT64_MAX), "standalone window fence wait");

    uint32_t img_idx = 0;
    const VkResult acquired = vkAcquireNextImageKHR(dev, swapchain_->handle(), UINT64_MAX,
                                                    image_available_[frame_index_], VK_NULL_HANDLE, &img_idx);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_->recreate();
        return;
    }
    if (acquired != VK_SUBOPTIMAL_KHR) vk_check(acquired, "standalone window acquire");
    vk_check(vkResetFences(dev, 1, &in_flight_[frame_index_]), "standalone window fence reset");

    const VkCommandBuffer cmd = command_buffers_[frame_index_];
    vk_check(vkResetCommandBuffer(cmd, 0), "standalone window command reset");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vk_check(vkBeginCommandBuffer(cmd, &begin), "standalone window command begin");

    // After the acquire, which the submit waits for at the colour output stage.
    vk_utils::transition_image(cmd, swapchain_->image(img_idx), VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = swapchain_->image_view(img_idx);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = CLEAR;

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = swapchain_->extent();
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);

    imgui_->activate();
    imgui_->begin_frame();
    draw(static_cast<float>(swapchain_->extent().width), static_cast<float>(swapchain_->extent().height));
    imgui_->end_frame();
    imgui_->render(cmd);
    imgui_->deactivate();

    vkCmdEndRendering(cmd);
    vk_utils::transition_image(cmd, swapchain_->image(img_idx), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    vk_check(vkEndCommandBuffer(cmd), "standalone window command end");

    const auto& render_done = swapchain_->render_finished_semaphores();
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &image_available_[frame_index_];
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_done[img_idx];
    vk_check(vkQueueSubmit(gpu_.graphics_queue(), 1, &submit, in_flight_[frame_index_]), "standalone window submit");

    const VkSwapchainKHR sc = swapchain_->handle();
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_done[img_idx];
    present.swapchainCount = 1;
    present.pSwapchains = &sc;
    present.pImageIndices = &img_idx;
    const VkResult presented = vkQueuePresentKHR(gpu_.graphics_queue(), &present);

    // A stale swapchain keeps its creation size while the window grows, which
    // leaves the UI laid out in a corner and the mouse landing away from it.
    // was_resized() covers compositors that resize without reporting
    // OUT_OF_DATE.
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR || window_->was_resized()) {
        window_->reset_resized();
        vk_check(vkDeviceWaitIdle(dev), "standalone window resize wait");
        swapchain_->recreate();
    } else {
        vk_check(presented, "standalone window present");
    }
    frame_index_ = (frame_index_ + 1) % MAX_FRAMES_IN_FLIGHT;
}

} // namespace fjell
