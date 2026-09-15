#include "ui/file_browser.hpp"
#include "core/log.hpp"
#include "core/window.hpp"
#include "renderer/gpu/gpu_core.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "ui/imgui_layer.hpp"


#include <imgui.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>

namespace fjell {

namespace fs = std::filesystem;

static constexpr int BROWSER_WIDTH = 720;
static constexpr int BROWSER_HEIGHT = 520;


FileBrowser::~FileBrowser() {
    close();
}

std::string FileBrowser::format_size(uintmax_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
    if (bytes < 1024 * 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + " MB";
    return std::to_string(bytes / (1024 * 1024 * 1024)) + " GB";
}

std::string FileBrowser::format_time(fs::file_time_type time) {
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    auto tt = std::chrono::system_clock::to_time_t(sctp);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&tt));
    return buf;
}

// ── Window lifecycle ────────────────────────────────────────────────────

void FileBrowser::open(const std::string& title, Mode mode,
                       const std::vector<std::string>& extensions) {
    if (!gpu_) {
        FJELL_CORE_ERROR("FileBrowser::open() called without set_gpu()");
        return;
    }

    if (is_open()) {
        close();
    }

    title_ = title;
    mode_ = mode;
    extensions_ = extensions;
    selected_index_ = -1;
    search_buf_[0] = '\0';
    name_buf_[0] = '\0';

    // Start in home directory
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home) home = std::getenv("USERPROFILE");
#endif
    if (home) {
        navigate(fs::path(home));
    } else {
        navigate(fs::current_path());
    }

    // Create standalone window
    window_ = std::make_unique<Window>(title, BROWSER_WIDTH, BROWSER_HEIGHT);
    surface_ = window_->create_surface(gpu_->instance());
    swapchain_ = std::make_unique<Swapchain>(
        gpu_->device(), gpu_->allocator(), *window_, surface_);

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

    imgui_ = std::make_unique<ImGuiLayer>(
        window_->handle(), gpu_->instance(),
        gpu_->physical_device(), gpu_->vk_device(),
        gpu_->graphics_family(), gpu_->graphics_queue(),
        swapchain_->format(), swapchain_->image_count());

    frame_index_ = 0;
}

void FileBrowser::tick() {
    if (!is_open()) return;

    if (window_->should_close()) {
        close();
        return;
    }

    auto dev = gpu_->vk_device();

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
    VkResult present_result = vkQueuePresentKHR(gpu_->graphics_queue(), &present);

    // A stale swapchain keeps its creation size while the window grows, which
    // leaves the UI laid out in a corner and the mouse landing away from it.
    // was_resized() covers compositors that resize without reporting
    // OUT_OF_DATE.
    if (present_result == VK_ERROR_OUT_OF_DATE_KHR ||
        present_result == VK_SUBOPTIMAL_KHR ||
        window_->was_resized()) {
        window_->reset_resized();
        vkDeviceWaitIdle(gpu_->vk_device());
        swapchain_->recreate();
    }

    frame_index_ = (frame_index_ + 1) % MAX_FRAMES_IN_FLIGHT;

    if (should_close) {
        close();
    }
}

void FileBrowser::close() {
    if (!is_open()) return;

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

// ── Directory operations ────────────────────────────────────────────────

void FileBrowser::navigate(const fs::path& dir) {
    try {
        current_dir_ = fs::canonical(dir);
    } catch (...) {
        current_dir_ = dir;
    }
    std::strncpy(path_buf_, current_dir_.string().c_str(), sizeof(path_buf_) - 1);
    path_buf_[sizeof(path_buf_) - 1] = '\0';
    selected_index_ = -1;
    refresh();
}

void FileBrowser::refresh() {
    entries_.clear();

    try {
        for (const auto& entry : fs::directory_iterator(
                 current_dir_, fs::directory_options::skip_permission_denied)) {
            auto name = entry.path().filename().string();
            if (name.empty()) continue;
            // Skip hidden files (but not in the root directory check)
            if (name[0] == '.' && name != "..") continue;

            bool is_dir = entry.is_directory();

            // In select_file mode with extension filter, skip non-matching files
            // (directories always pass through)
            if (!is_dir && mode_ == Mode::select_file && !extensions_.empty()) {
                auto ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                bool matches = false;
                for (const auto& e : extensions_) {
                    if (ext == e) { matches = true; break; }
                }
                if (!matches) continue;
            }

            // In select_directory/select_location mode, only show directories
            if (!is_dir && mode_ != Mode::select_file) continue;

            uintmax_t size = 0;
            fs::file_time_type modified{};
            try {
                if (!is_dir) size = entry.file_size();
                modified = entry.last_write_time();
            } catch (...) {}

            entries_.push_back({name, is_dir, size, modified});
        }
    } catch (...) {}

    // Apply current sort
    auto comparator = [this](const Entry& a, const Entry& b) -> bool {
        // Directories always first
        if (a.is_directory != b.is_directory) return a.is_directory;

        bool less = false;
        switch (sort_column_) {
            case SortColumn::name: less = a.name < b.name; break;
            case SortColumn::size: less = a.size < b.size; break;
            case SortColumn::date: less = a.modified < b.modified; break;
        }
        return sort_ascending_ ? less : !less;
    };
    std::sort(entries_.begin(), entries_.end(), comparator);

    // Rebuild filtered indices
    filtered_indices_.clear();
    std::string filter(search_buf_);
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

    for (size_t i = 0; i < entries_.size(); ++i) {
        if (filter.empty()) {
            filtered_indices_.push_back(i);
            continue;
        }
        std::string lower_name = entries_[i].name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
        if (lower_name.find(filter) != std::string::npos) {
            filtered_indices_.push_back(i);
        }
    }
}

void FileBrowser::confirm_selection() {
    if (mode_ == Mode::select_file) {
        // Must have a file selected
        if (selected_index_ < 0 || selected_index_ >= static_cast<int>(filtered_indices_.size()))
            return;
        const auto& entry = entries_[filtered_indices_[selected_index_]];
        if (entry.is_directory) return;
        auto path = (current_dir_ / entry.name).string();
        on_selected.broadcast(path);
    } else if (mode_ == Mode::select_directory) {
        // Use the selected directory, or current directory if none selected
        std::string path = current_dir_.string();
        if (selected_index_ >= 0 && selected_index_ < static_cast<int>(filtered_indices_.size())) {
            const auto& entry = entries_[filtered_indices_[selected_index_]];
            if (entry.is_directory) {
                path = (current_dir_ / entry.name).string();
            }
        }
        on_selected.broadcast(path);
    } else if (mode_ == Mode::select_location) {
        if (name_buf_[0] == '\0') return;
        on_location_selected.broadcast(current_dir_.string(), std::string(name_buf_));
    }

    glfwSetWindowShouldClose(window_->handle(), GLFW_TRUE);
}

// ── ImGui UI ────────────────────────────────────────────────────────────

bool FileBrowser::draw_ui(float window_w, float window_h) {
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({window_w, window_h});
    ImGui::Begin("##FileBrowser", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    draw_path_bar();
    draw_search_bar();
    ImGui::Separator();
    draw_file_list();
    ImGui::Separator();
    draw_bottom_bar();

    ImGui::End();
    return false; // close is handled by glfwSetWindowShouldClose
}

void FileBrowser::draw_path_bar() {
    // Up button
    if (ImGui::Button("^", {24, 0})) {
        auto parent = current_dir_.parent_path();
        if (parent != current_dir_) {
            navigate(parent);
        }
    }
    ImGui::SameLine();

    // Editable path
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##path", path_buf_, sizeof(path_buf_),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        fs::path typed(path_buf_);
        if (fs::is_directory(typed)) {
            navigate(typed);
        }
    }
}

void FileBrowser::draw_search_bar() {
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##search", "Search...", search_buf_, sizeof(search_buf_))) {
        // Rebuild filtered indices on every keystroke
        filtered_indices_.clear();
        std::string filter(search_buf_);
        std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (filter.empty()) {
                filtered_indices_.push_back(i);
                continue;
            }
            std::string lower_name = entries_[i].name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
            if (lower_name.find(filter) != std::string::npos) {
                filtered_indices_.push_back(i);
            }
        }
        selected_index_ = -1;
    }
}

void FileBrowser::draw_file_list() {
    float bottom_height = (mode_ == Mode::select_location) ? 64.0f : 36.0f;
    float list_h = ImGui::GetContentRegionAvail().y - bottom_height - 8.0f;

    ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerV;

    bool show_size = (mode_ == Mode::select_file);
    int col_count = show_size ? 3 : 2;

    if (ImGui::BeginTable("##files", col_count, flags, {0, list_h})) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch);
        if (show_size) {
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        }
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // Handle sorting
        if (auto* sort_specs = ImGui::TableGetSortSpecs()) {
            if (sort_specs->SpecsDirty && sort_specs->SpecsCount > 0) {
                auto& spec = sort_specs->Specs[0];
                if (spec.ColumnIndex == 0) sort_column_ = SortColumn::name;
                else if (show_size && spec.ColumnIndex == 1) sort_column_ = SortColumn::size;
                else sort_column_ = SortColumn::date;
                sort_ascending_ = (spec.SortDirection == ImGuiSortDirection_Ascending);
                sort_specs->SpecsDirty = false;
                refresh();
            }
        }

        for (int fi = 0; fi < static_cast<int>(filtered_indices_.size()); ++fi) {
            const auto& entry = entries_[filtered_indices_[fi]];

            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            // Icon: use provider callback if available, colored text fallback otherwise
            if (icon_provider_) {
                auto tex = icon_provider_(entry);
                if (tex) {
                    float icon_size = ImGui::GetTextLineHeight();
                    ImGui::Image(tex, {icon_size, icon_size});
                    ImGui::SameLine();
                }
            } else {
                if (entry.is_directory) {
                    ImGui::TextColored({0.831f, 0.627f, 0.329f, 1.0f}, "D");
                } else {
                    ImGui::TextDisabled("F");
                }
                ImGui::SameLine();
            }

            bool selected = (fi == selected_index_);
            ImGui::PushID(fi);
            if (ImGui::Selectable(entry.name.c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                selected_index_ = fi;

                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (entry.is_directory) {
                        navigate(current_dir_ / entry.name);
                    } else if (mode_ == Mode::select_file) {
                        confirm_selection();
                    }
                }
            }
            ImGui::PopID();

            // Size column
            if (show_size) {
                ImGui::TableNextColumn();
                if (!entry.is_directory) {
                    ImGui::TextDisabled("%s", format_size(entry.size).c_str());
                }
            }

            // Date column
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", format_time(entry.modified).c_str());
        }

        ImGui::EndTable();
    }
}

void FileBrowser::draw_bottom_bar() {
    if (mode_ == Mode::select_location) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 180.0f);
        ImGui::InputTextWithHint("##name", "Project name", name_buf_, sizeof(name_buf_));
        ImGui::SameLine();
    }

    bool can_confirm = true;
    if (mode_ == Mode::select_location && name_buf_[0] == '\0') {
        can_confirm = false;
    }
    if (mode_ == Mode::select_file) {
        // Need a file selected
        if (selected_index_ < 0 || selected_index_ >= static_cast<int>(filtered_indices_.size()) ||
            entries_[filtered_indices_[selected_index_]].is_directory) {
            can_confirm = false;
        }
    }

    if (!can_confirm) ImGui::BeginDisabled();
    if (ImGui::Button("Select", {80, 0})) {
        confirm_selection();
    }
    if (!can_confirm) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", {80, 0})) {
        glfwSetWindowShouldClose(window_->handle(), GLFW_TRUE);
    }
}

} // namespace fjell
