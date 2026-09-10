#include "import/import_dialog.hpp"
#include "core/log.hpp"
#include "core/window.hpp"
#include "renderer/gpu/gpu_core.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "ui/imgui_layer.hpp"

#include "platform/platform.hpp"

#include <imgui.h>
#include <GLFW/glfw3.h>

#include <cstring>
#include <filesystem>

namespace fjell {

namespace fs = std::filesystem;

static constexpr int DIALOG_WIDTH = 500;
static constexpr int DIALOG_HEIGHT = 650;

ImportDialog::~ImportDialog() {
    close();
}

std::string ImportDialog::resolve_font_dir() {
    auto exe_dir = platform::executable_path().parent_path();
    for (int i = 0; i < 3; ++i) {
        if (fs::is_directory(exe_dir / "engine_assets" / "fonts")) {
            return (exe_dir / "engine_assets" / "fonts").string();
        }
        exe_dir = exe_dir.parent_path();
    }
    return {};
}

void ImportDialog::open(const fs::path& source_path, const fs::path& default_output_dir) {
    if (!gpu_) {
        FJELL_CORE_ERROR("ImportDialog::open() called without set_gpu()");
        return;
    }

    if (is_open()) {
        // Already open — update the source file
        settings_.project_root = project_root_;
        settings_.texture_dir = project_texture_dir_;
        settings_.source_path = source_path;
        settings_.output_dir = default_output_dir;
        source_filename_ = source_path.filename().string();
        std::snprintf(output_dir_buf_, sizeof(output_dir_buf_), "%s",
                      default_output_dir.string().c_str());
        return;
    }

    settings_ = MeshImportSettings{};
    settings_.project_root = project_root_;
    settings_.texture_dir = project_texture_dir_;
    settings_.source_path = source_path;
    settings_.output_dir = default_output_dir;
    source_filename_ = source_path.filename().string();
    std::snprintf(output_dir_buf_, sizeof(output_dir_buf_), "%s",
                  default_output_dir.string().c_str());
    std::snprintf(texture_dir_buf_, sizeof(texture_dir_buf_), "%s",
                  settings_.texture_dir.c_str());

    // Create GLFW window (no Vulkan hint — we share the editor's instance)
    std::string title = "Import: " + source_filename_;
    window_ = std::make_unique<Window>(title, DIALOG_WIDTH, DIALOG_HEIGHT);

    // Create a surface for this window on the editor's Vulkan instance
    surface_ = window_->create_surface(gpu_->instance());

    // Create swapchain on the shared device, using our surface
    swapchain_ = std::make_unique<Swapchain>(
        gpu_->device(), gpu_->allocator(), *window_, surface_);

    // Command pool + buffers + sync (on the shared device)
    auto dev = gpu_->vk_device();

    VkCommandPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_ci.queueFamilyIndex = gpu_->graphics_family();
    vkCreateCommandPool(dev, &pool_ci, nullptr, &command_pool_);

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    vkAllocateCommandBuffers(dev, &alloc_info, command_buffers_.data());

    VkSemaphoreCreateInfo sem_ci{};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fence_ci{};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        vkCreateSemaphore(dev, &sem_ci, nullptr, &image_available_[i]);
        vkCreateFence(dev, &fence_ci, nullptr, &in_flight_[i]);
    }

    // ImGui layer — creates its own context, shares the device
    imgui_ = std::make_unique<ImGuiLayer>(
        window_->handle(), gpu_->instance(),
        gpu_->physical_device(), gpu_->vk_device(),
        gpu_->graphics_family(), gpu_->graphics_queue(),
        swapchain_->format(), swapchain_->image_count(),
        resolve_font_dir());

    // Wire up import completion
    job_connection_ = job_.on_complete.bind([this](bool success, const std::vector<std::string>& files) {
        on_import_complete.broadcast(success, files);
    });

    frame_index_ = 0;
    FJELL_CORE_INFO("Import dialog opened for {}", source_filename_);
}

void ImportDialog::tick() {
    if (!is_open()) return;

    // Check if the user closed the window
    if (window_->should_close()) {
        close();
        return;
    }

    // Poll import progress
    job_.poll();

    auto dev = gpu_->vk_device();

    // Frame sync
    vkWaitForFences(dev, 1, &in_flight_[frame_index_], VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &in_flight_[frame_index_]);

    uint32_t img_idx = 0;
    auto result = vkAcquireNextImageKHR(
        dev, swapchain_->handle(), UINT64_MAX,
        image_available_[frame_index_], VK_NULL_HANDLE, &img_idx);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_->recreate();
        return;
    }

    auto cmd = command_buffers_[frame_index_];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);

    vk_utils::prepare_color_attachment(cmd, swapchain_->image(img_idx));

    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = swapchain_->image_view(img_idx);
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.clearValue.color = {{0.012f, 0.012f, 0.015f, 1.0f}};

    VkRenderingInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea.extent = swapchain_->extent();
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachments = &color_attachment;

    vkCmdBeginRendering(cmd, &rendering_info);

    // ImGui frame — switch to our context
    imgui_->activate();
    imgui_->begin_frame();
    bool should_close = draw_ui(
        static_cast<float>(swapchain_->extent().width),
        static_cast<float>(swapchain_->extent().height));
    imgui_->end_frame();
    imgui_->render(cmd);
    imgui_->deactivate();

    vkCmdEndRendering(cmd);

    vk_utils::transition_image(cmd, swapchain_->image(img_idx),
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

    vkEndCommandBuffer(cmd);

    const auto& render_done = swapchain_->render_finished_semaphores();
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &image_available_[frame_index_];
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_done[img_idx];

    vkQueueSubmit(gpu_->graphics_queue(), 1, &submit, in_flight_[frame_index_]);

    VkSwapchainKHR sc = swapchain_->handle();
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_done[img_idx];
    present.swapchainCount = 1;
    present.pSwapchains = &sc;
    present.pImageIndices = &img_idx;
    vkQueuePresentKHR(gpu_->graphics_queue(), &present);

    frame_index_ = (frame_index_ + 1) % MAX_FRAMES_IN_FLIGHT;

    if (should_close) {
        close();
    }
}

void ImportDialog::close() {
    if (!is_open()) return;

    if (job_.state() == ImportState::running) {
        job_.cancel();
    }

    auto dev = gpu_->vk_device();
    vkDeviceWaitIdle(dev);

    imgui_.reset();

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (image_available_[i] != VK_NULL_HANDLE)
            vkDestroySemaphore(dev, image_available_[i], nullptr);
        if (in_flight_[i] != VK_NULL_HANDLE)
            vkDestroyFence(dev, in_flight_[i], nullptr);
        image_available_[i] = VK_NULL_HANDLE;
        in_flight_[i] = VK_NULL_HANDLE;
    }
    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(dev, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }

    swapchain_.reset();

    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(gpu_->instance(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    window_.reset();
    frame_index_ = 0;
}

// ── ImGui UI ────────────────────────────────────────────────────────────

bool ImportDialog::draw_ui(float window_w, float window_h) {
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({window_w, window_h});
    ImGui::Begin("##ImportDialog", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    bool should_close = false;

    draw_source_info();
    ImGui::Separator();
    draw_output_settings();
    ImGui::Separator();

    bool running = job_.state() == ImportState::running;

    if (!running) {
        draw_lod_settings();
        ImGui::Spacing();
        draw_sdf_settings();
        ImGui::Spacing();
        draw_animation_settings();
        ImGui::Spacing();
        draw_texture_settings();
    }

    ImGui::Separator();

    if (running) {
        draw_progress();
    }

    draw_action_buttons();

    if (job_.state() == ImportState::succeeded || job_.state() == ImportState::failed) {
        ImGui::Spacing();
        if (job_.state() == ImportState::succeeded) {
            ImGui::TextColored({0.494f, 0.749f, 0.557f, 1.0f}, "Import complete!");
        } else {
            ImGui::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, "Import failed.");
        }
    }

    ImGui::End();
    return should_close;
}

void ImportDialog::draw_source_info() {
    ImGui::TextColored({0.831f, 0.627f, 0.329f, 1.0f}, "Source");
    ImGui::Spacing();
    ImGui::Text("%s", source_filename_.c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", settings_.source_path.string().c_str());
    }
}

void ImportDialog::draw_output_settings() {
    ImGui::TextColored({0.831f, 0.627f, 0.329f, 1.0f}, "Output");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##output_dir", output_dir_buf_, sizeof(output_dir_buf_));
    settings_.output_dir = output_dir_buf_;
}

void ImportDialog::draw_lod_settings() {
    ImGui::TextColored({0.831f, 0.627f, 0.329f, 1.0f}, "LOD Generation");
    ImGui::Spacing();

    ImGui::Checkbox("Generate LODs", &settings_.generate_lods);

    if (settings_.generate_lods) {
        int lod_count = static_cast<int>(settings_.lod_count);
        ImGui::SliderInt("LOD Levels", &lod_count, 1, 8);
        settings_.lod_count = static_cast<uint32_t>(lod_count);

        if (ImGui::TreeNode("LOD Details")) {
            for (uint32_t i = 0; i < settings_.lod_count; ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::Text("LOD %u:", i);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                ImGui::SliderFloat("Threshold", &settings_.lod_thresholds[i], 0.01f, 1.0f, "%.2f");
                if (i > 0) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(100);
                    ImGui::SliderFloat("Ratio", &settings_.lod_target_ratios[i], 0.05f, 1.0f, "%.2f");
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }
}

void ImportDialog::draw_sdf_settings() {
    ImGui::TextColored({0.831f, 0.627f, 0.329f, 1.0f}, "Signed Distance Field");
    ImGui::Spacing();

    ImGui::Checkbox("Generate SDF", &settings_.generate_sdf);
    if (settings_.generate_sdf) {
        int res = static_cast<int>(settings_.sdf_resolution);
        ImGui::SliderInt("Resolution", &res, 8, 128);
        settings_.sdf_resolution = static_cast<uint32_t>(res);
    }
}

void ImportDialog::draw_animation_settings() {
    ImGui::TextColored({0.831f, 0.627f, 0.329f, 1.0f}, "Animation");
    ImGui::Spacing();
    ImGui::Checkbox("Import Animations", &settings_.import_animations);
}

void ImportDialog::draw_texture_settings() {
    ImGui::TextColored({0.831f, 0.627f, 0.329f, 1.0f}, "Textures");
    ImGui::Spacing();

    ImGui::Checkbox("Import Textures", &settings_.import_textures);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Extract textures the source file references and write\n"
                          "a .fjmat per material next to the .fjmesh.");
    }

    if (settings_.import_textures) {
        if (ImGui::InputText("Texture Folder", texture_dir_buf_, sizeof(texture_dir_buf_))) {
            settings_.texture_dir = texture_dir_buf_;
        }
    }
}

void ImportDialog::draw_action_buttons() {
    ImGui::Spacing();

    bool running = job_.state() == ImportState::running;

    if (running) {
        if (ImGui::Button("Cancel", {120, 32})) {
            job_.cancel();
        }
    } else {
        if (ImGui::Button("Import", {120, 32})) {
            settings_.output_dir = output_dir_buf_;
            job_.start(settings_);
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", {120, 32})) {
            glfwSetWindowShouldClose(window_->handle(), GLFW_TRUE);
        }
    }
}

void ImportDialog::draw_progress() {
    const auto& p = job_.progress();
    ImGui::ProgressBar(static_cast<float>(p.percent) / 100.0f, {-1, 0}, p.message.c_str());
}

} // namespace fjell
