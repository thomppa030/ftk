#pragma once

#include "import/import_types.hpp"
#include "import/import_job.hpp"
#include "import/source_inspection.hpp"
#include "ftk/base/delegate.hpp"
#include "ftk/app/standalone_window.hpp"


#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

class ImGuiLayer;
class Window;

/// Standalone OS window for configuring and running mesh imports.
/// Shares the editor's GPU device but creates its own window, swapchain and
/// ImGui context. Call tick()
/// from the engine main loop each frame.
class ImportDialog {
public:
    ImportDialog() = default;
    ~ImportDialog();

    ImportDialog(const ImportDialog&) = delete;
    ImportDialog& operator=(const ImportDialog&) = delete;

    /// Set the device to share. Must be called before open().
    void set_device(gpu::Device* device) { device_ = device; }

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

    /// Fired when the user asks to see an imported file in the content browser.
    Delegate<void(const std::filesystem::path& file)> on_show_result;

private:
    /// Draw the ImGui UI. Returns true if the window should close.
    bool draw_ui(float window_w, float window_h);
    void draw_form();
    void draw_summary();
    /// The bottom bar; true when the window should close.
    bool draw_bottom_bar();

    // Shared from the editor (not owned)
    gpu::Device* device_{nullptr};
    std::filesystem::path project_root_;
    std::string project_texture_dir_{"assets/textures"};

    std::unique_ptr<StandaloneWindow> window_;

    // Import state
    MeshImportSettings settings_;
    ImportJob job_;
    Connection job_connection_;
    SourceInspection inspection_;

    std::string source_filename_;
    std::string output_dir_;
    std::string texture_dir_;
};

} // namespace fjell
