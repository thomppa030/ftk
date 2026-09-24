#include "import/import_dialog.hpp"
#include "core/log.hpp"
#include "ui/standalone_window.hpp"


#include <imgui.h>

#include <cstring>
#include <filesystem>

namespace fjell {

namespace fs = std::filesystem;

static constexpr int DIALOG_WIDTH = 500;
static constexpr int DIALOG_HEIGHT = 650;

ImportDialog::~ImportDialog() {
    close();
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

    window_ = std::make_unique<StandaloneWindow>(*gpu_, "Import: " + source_filename_, DIALOG_WIDTH, DIALOG_HEIGHT);

    // Wire up import completion
    job_connection_ = job_.on_complete.bind([this](bool success, const std::vector<std::string>& files) {
        on_import_complete.broadcast(success, files);
    });

    FJELL_CORE_INFO("Import dialog opened for {}", source_filename_);
}

void ImportDialog::tick() {
    if (!is_open()) return;
    if (window_->close_requested()) {
        close();
        return;
    }
    job_.poll();
    bool should_close = false;
    window_->frame([&](float width, float height) { should_close = draw_ui(width, height); });
    if (should_close) close();
}

void ImportDialog::close() {
    if (!is_open()) return;
    if (job_.state() == ImportState::running) job_.cancel();
    window_.reset();
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
            window_->request_close();
        }
    }
}

void ImportDialog::draw_progress() {
    const auto& p = job_.progress();
    ImGui::ProgressBar(static_cast<float>(p.percent) / 100.0f, {-1, 0}, p.message.c_str());
}

} // namespace fjell
