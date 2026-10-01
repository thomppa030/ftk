#include "import/import_dialog.hpp"

#include "ftk/base/log.hpp"
#include "ftk/ui/kit/brand.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/dialog.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/pane.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/text_field.hpp"
#include "ui/engine_imgui_files.hpp"
#include "ftk/ui/kit/viewport_toolbar.hpp"
#include "ftk/app/standalone_window.hpp"
#include "ftk/ui/theme.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

namespace fjell {

namespace fs = std::filesystem;

namespace {

constexpr int DIALOG_WIDTH = 760;
constexpr int DIALOG_HEIGHT = 620;
constexpr float SUMMARY_W = 250.0f;

// A count with its thousands apart: 12,480.
std::string grouped(uint64_t n) {
    std::string digits = std::to_string(n);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<size_t>(i), ",");
    return digits;
}

} // namespace

ImportDialog::~ImportDialog() {
    close();
}

void ImportDialog::open(const fs::path& source_path, const fs::path& default_output_dir) {
    if (!device_) {
        FJELL_CORE_ERROR("ImportDialog::open() called without set_device()");
        return;
    }

    // A new source keeps the settings of an open dialog; a new dialog starts
    // from the defaults.
    if (!is_open()) settings_ = MeshImportSettings{};
    settings_.project_root = project_root_;
    settings_.texture_dir = project_texture_dir_;
    settings_.source_path = source_path;
    settings_.output_dir = default_output_dir;
    source_filename_ = source_path.filename().string();
    output_dir_ = default_output_dir.string();
    texture_dir_ = settings_.texture_dir;
    inspection_.start(source_path);
    if (is_open()) return;

    window_ = std::make_unique<StandaloneWindow>(*device_, "Import: " + source_filename_, DIALOG_WIDTH, DIALOG_HEIGHT,
                                                 engine_imgui_files());
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
    inspection_.poll();
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
    (void)ui::begin_host_window("##ImportDialog", ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                                      ImGuiWindowFlags_NoScrollbar);
    const float bar_h = ImGui::GetFrameHeight() + theme::GAP_M * 2.0f + 1.0f;
    const float body_h = std::max(ImGui::GetContentRegionAvail().y - bar_h, 1.0f);
    if (auto form = ui::Pane("##form", {std::max(window_w - SUMMARY_W, 1.0f), body_h})) draw_form();
    ImGui::SameLine(0.0f, 0.0f);
    if (auto summary = ui::Pane("##summary", {0.0f, body_h}, ui::PaneSurface::Sunken)) draw_summary();
    ImGui::SetCursorPosX(theme::GAP_L);
    const bool close = draw_bottom_bar();
    ImGui::End();
    return close;
}

void ImportDialog::draw_form() {
    const bool running = job_.state() == ImportState::running;
    if (job_.state() == ImportState::failed) {
        const std::string why = job_.progress().message.empty() ? "fjimport stopped; the console says why."
                                                                : job_.progress().message;
        (void)ui::callout(ui::Severity::Error, "The import failed", why.c_str());
    }
    ImGui::BeginDisabled(running);

    ui::section("Source", ui::icon::open_file);
    if (auto t = ui::PropertyTable("##source")) {
        ui::row("File", [&] {
            ui::readout(source_filename_.c_str());
            ImGui::SetItemTooltip("%s", settings_.source_path.string().c_str());
        });
        ui::row("Output", "The folder the .fjmesh and what comes with it are written to.", [&] {
            if (ui::text_field("##output", output_dir_).committed) settings_.output_dir = output_dir_;
        });
    }

    ui::section("Level of detail", ui::icon::surface);
    if (auto t = ui::PropertyTable("##lod")) {
        ui::row("Generate LODs", "Simpler versions of the mesh, drawn as it gets smaller on screen.",
                [&] { (void)ui::checkbox("##lods", settings_.generate_lods); });
        if (settings_.generate_lods) {
            ui::row("Levels", [&] {
                int count = static_cast<int>(settings_.lod_count);
                if (ui::slider_int("##levels", count, 1, 8).changed) settings_.lod_count = static_cast<uint32_t>(count);
            });
        }
    }
    if (settings_.generate_lods) {
        ui::subheading("Levels");
        if (auto t = ui::PropertyTable("##levels_table")) {
            for (uint32_t i = 0; i < settings_.lod_count; ++i) {
                char label[24];
                std::snprintf(label, sizeof(label), "Level %u", i + 1);
                ImGui::PushID(static_cast<int>(i));
                ui::row(label, "Drawn while the mesh covers more than this share of the screen, with this share of "
                               "its triangles.",
                        [&] {
                            const float half = (ImGui::GetContentRegionAvail().x - theme::GAP_S) * 0.5f;
                            ImGui::SetNextItemWidth(half);
                            (void)ui::drag("##threshold", settings_.lod_thresholds[i],
                                           {.speed = 0.005f, .min = 0.01f, .max = 1.0f, .format = "%.2f of screen"});
                            ImGui::SameLine(0.0f, theme::GAP_S);
                            ImGui::SetNextItemWidth(half);
                            if (i == 0) {
                                ui::readout("every triangle");
                            } else {
                                float percent = settings_.lod_target_ratios[i] * 100.0f;
                                if (ui::drag("##ratio", percent, {.speed = 0.5f, .min = 5.0f, .max = 100.0f,
                                                                  .format = "%.0f%% of triangles"})
                                        .changed) {
                                    settings_.lod_target_ratios[i] = percent / 100.0f;
                                }
                            }
                        });
                ImGui::PopID();
            }
        }
    }

    ui::section("Distance field", ui::icon::collision);
    if (auto t = ui::PropertyTable("##sdf")) {
        ui::row("Generate SDF", "A distance field of the mesh, for particles and lighting that need its shape.",
                [&] { (void)ui::checkbox("##sdf", settings_.generate_sdf); });
        if (settings_.generate_sdf) {
            ui::row("Resolution", "Cells along each side.", [&] {
                int res = static_cast<int>(settings_.sdf_resolution);
                if (ui::slider_int("##resolution", res, 8, 128).changed) {
                    settings_.sdf_resolution = static_cast<uint32_t>(res);
                }
            });
        }
    }

    ui::section("Animation", ui::icon::animation);
    if (auto t = ui::PropertyTable("##animation")) {
        ui::row("Import animations", [&] { (void)ui::checkbox("##animations", settings_.import_animations); });
    }

    ui::section("Textures", ui::icon::texture);
    if (auto t = ui::PropertyTable("##textures")) {
        ui::row("Import textures", "Extract the textures the file's materials use and write a .fjmat per material.",
                [&] { (void)ui::checkbox("##textures", settings_.import_textures); });
        if (settings_.import_textures) {
            ui::row("Texture folder", "Where the textures are written, under the project.", [&] {
                if (ui::text_field("##texture_dir", texture_dir_).committed) settings_.texture_dir = texture_dir_;
            });
        }
    }
    if (!settings_.import_textures) ui::hint("Materials will point at textures already in the project.");
    ImGui::EndDisabled();
}

void ImportDialog::draw_summary() {
    ui::section("In this file", ui::icon::info);
    switch (inspection_.state()) {
        case SourceInspection::State::idle:
        case SourceInspection::State::reading: {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ui::spinner(ImGui::GetWindowDrawList(), {at.x + 10.0f, at.y + 10.0f}, 8.0f,
                        static_cast<float>(ImGui::GetTime()));
            ImGui::SetCursorScreenPos({at.x + 28.0f, at.y + 2.0f});
            ImGui::TextColored(theme::text_secondary(), "Reading the file\xe2\x80\xa6");
            return;
        }
        case SourceInspection::State::failed:
            ui::status(ui::Severity::Error, "The file couldn't be read; the console says why.");
            return;
        case SourceInspection::State::read: break;
    }

    const SourceSummary& s = inspection_.summary();
    std::string textures = "none";
    if (s.embedded_textures > 0 && s.texture_files > 0) {
        textures = std::to_string(s.embedded_textures) + " embedded, " + std::to_string(s.texture_files) + " files";
    } else if (s.embedded_textures > 0) {
        textures = std::to_string(s.embedded_textures) + " embedded";
    } else if (s.texture_files > 0) {
        textures = std::to_string(s.texture_files) + (s.texture_files == 1 ? " file" : " files");
    }
    const std::string meshes = std::to_string(s.meshes);
    const std::string triangles = grouped(s.triangles);
    const std::string materials = std::to_string(s.materials);
    const std::string skeleton = s.bones > 0 ? std::to_string(s.bones) + " bones" : "none";
    const std::string animations = s.animations > 0 ? std::to_string(s.animations) : "none";
    if (auto t = ui::PropertyTable("##summary_rows")) {
        ui::row("Meshes", [&] { ui::readout(meshes.c_str()); });
        ui::row("Triangles", [&] { ui::readout(triangles.c_str()); });
        ui::row("Materials", [&] { ui::readout(materials.c_str()); });
        ui::row("Textures", [&] { ui::readout(textures.c_str()); });
        ui::row("Skeleton", [&] { ui::readout(skeleton.c_str()); });
        ui::row("Animations", [&] { ui::readout(animations.c_str()); });
    }

    // Settings the file gives nothing to do.
    ImGui::Dummy({0.0f, theme::GAP_S});
    if (settings_.import_animations && s.animations == 0) {
        ui::status(ui::Severity::Warning, "Import animations is on, but the file has none.");
    }
    if (settings_.import_textures && s.embedded_textures == 0 && s.texture_files == 0) {
        ui::status(ui::Severity::Warning, "Import textures is on, but the file has none.");
    }
    if (s.bones > 0 && (settings_.generate_lods || settings_.generate_sdf)) {
        ui::status(ui::Severity::Warning, "The model is skinned: it gets no LODs or distance field.");
    }
}

bool ImportDialog::draw_bottom_bar() {
    const bool running = job_.state() == ImportState::running;
    const auto answer = ui::window_bar(
        {.title = "Import model", .confirm = "Import"},
        {.can_confirm = !running && !output_dir_.empty(), .why_not = running ? "Importing" : "Choose an output folder",
         .focus_confirm = false},
        [&](float width) {
            if (running) {
                const auto& p = job_.progress();
                char label[160];
                std::snprintf(label, sizeof(label), "%s %d%%", p.message.c_str(), p.percent);
                ImGui::ProgressBar(static_cast<float>(p.percent) / 100.0f, {width, 0.0f}, label);
            } else if (job_.state() == ImportState::succeeded && !job_.output_files().empty()) {
                const fs::path first = job_.output_files().front();
                const std::string done = "Imported " + first.filename().string();
                ImGui::AlignTextToFramePadding();
                ui::status(ui::Severity::Success, done.c_str());
                ImGui::SameLine();
                if (ui::action(ui::icon::show_in_files, "Show in content browser", ui::ButtonKind::Ghost)) {
                    on_show_result.broadcast(first);
                }
            } else {
                ImGui::Dummy({width, 0.0f});
            }
        });
    if (answer == ui::DialogAnswer::Confirm) {
        settings_.output_dir = output_dir_;
        settings_.texture_dir = texture_dir_;
        (void)job_.start(settings_);
    }
    if (answer == ui::DialogAnswer::Cancel) {
        // Cancel stops an import first; a second press closes.
        if (running) {
            job_.cancel();
            return false;
        }
        return true;
    }
    return false;
}

} // namespace fjell
