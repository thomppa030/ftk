#pragma once

#include "import/import_types.hpp"
#include "import/import_job.hpp"
#include "core/delegate.hpp"
#include "renderer/gpu/swapchain.hpp"
#include "renderer/renderer_constants.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

class GpuCore;
class ImGuiLayer;
class Window;

/// Standalone OS window for configuring and running mesh imports.
/// Shares the editor's Vulkan device (via GpuCore) but creates its own
/// window, surface, swapchain, command pool, and ImGui context. Call tick()
/// from the engine main loop each frame.
class ImportDialog {
public:
    ImportDialog() = default;
    ~ImportDialog();

    ImportDialog(const ImportDialog&) = delete;
    ImportDialog& operator=(const ImportDialog&) = delete;

    /// Set the GpuCore to share. Must be called before open().
    void set_gpu(GpuCore* gpu) { gpu_ = gpu; }

    /// Project root and the project's texture import directory. Texture paths
    /// in generated .fjmat files are written relative to the root. Set before
    /// open().
    void set_project(std::filesystem::path root, std::string texture_dir) {
        project_root_ = std::move(root);
        project_texture_dir_ = std::move(texture_dir);
    }

    /// Open the dialog for a source file. Creates the window if not already open.
    void open(const std::filesystem::path& source_path,
              const std::filesystem::path& default_output_dir);

    /// Render one frame. Call from the engine main loop if is_open().
    void tick();

    /// Close the dialog and destroy the window.
    void close();

    [[nodiscard]] bool is_open() const { return window_ != nullptr; }

    /// Fired on the main thread when an import finishes.
    Delegate<void(bool success, const std::vector<std::string>& output_files)> on_import_complete;

private:
    /// Draw the ImGui UI. Returns true if the window should close.
    bool draw_ui(float window_w, float window_h);

    void draw_source_info();
    void draw_output_settings();
    void draw_lod_settings();
    void draw_sdf_settings();
    void draw_animation_settings();
    void draw_texture_settings();
    void draw_action_buttons();
    void draw_progress();


    // Shared from the editor (not owned)
    GpuCore* gpu_{nullptr};
    std::filesystem::path project_root_;
    std::string project_texture_dir_{"assets/textures"};

    // Owned per-dialog: window + surface + swapchain + command pool + sync
    std::unique_ptr<Window> window_;
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    std::unique_ptr<Swapchain> swapchain_;
    std::unique_ptr<ImGuiLayer> imgui_;
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    std::array<VkCommandBuffer, MAX_FRAMES_IN_FLIGHT> command_buffers_{};
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> image_available_{};
    std::array<VkFence, MAX_FRAMES_IN_FLIGHT> in_flight_{};

    // Import state
    MeshImportSettings settings_;
    ImportJob job_;
    Connection job_connection_;
    uint32_t frame_index_{0};

    // Source file info (detected on open)
    std::string source_filename_;
    std::string source_info_;
    bool detected_skinned_{false};

    // Output dir as editable char buffer
    char output_dir_buf_[512]{};
    char texture_dir_buf_[128]{};
};

} // namespace fjell
