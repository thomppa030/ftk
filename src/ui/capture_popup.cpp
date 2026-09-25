#include "ui/capture_popup.hpp"

#include "core/capture_service.hpp"
#include "renderer/camera.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/dialog.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/row.hpp"
#include "ui/viewport_manager.hpp"

#include <glm/vec2.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace fjell {

namespace {

struct ResolutionPreset {
    const char* label;
    uint32_t w;
    uint32_t h;
};

constexpr std::array<ResolutionPreset, 5> kResolutionPresets{{
    {"1080p, 1920 \xc3\x97 1080", 1920, 1080},
    {"1440p, 2560 \xc3\x97 1440", 2560, 1440},
    {"4K, 3840 \xc3\x97 2160",    3840, 2160},
    {"8K, 7680 \xc3\x97 4320",    7680, 4320},
    {"Custom",                    0,    0},
}};

constexpr const char* DIALOG_ID = "##capture";

inline constexpr ui::Choice<int> MODES[] = {{0, "Scene"}, {1, "Pane"}};

} // namespace

void CapturePopup::refresh_pane_list() {
    pane_options_.clear();
    auto* ctx = ImGui::GetCurrentContext();
    if (ctx == nullptr) { return; }
    for (ImGuiWindow* w : ctx->Windows) {
        if (w == nullptr || w->Hidden || w->IsFallbackWindow != 0) { continue; }
        if (w->Flags & ImGuiWindowFlags_ChildWindow) { continue; }
        if (w->ParentWindow != nullptr) { continue; }
        if (w->Size.x <= 1 || w->Size.y <= 1) { continue; }
        if (std::strncmp(w->Name, "##", 2) == 0) { continue; }
        pane_options_.emplace_back(w->Name);
    }
    std::sort(pane_options_.begin(), pane_options_.end());
    if (pane_index_ >= static_cast<int>(pane_options_.size())) {
        pane_index_ = 0;
    }
}

void CapturePopup::draw(CaptureService& capture, ViewportManager& viewports) {
    if (want_open_) {
        refresh_pane_list();
        ui::open_dialog(DIALOG_ID);
        want_open_ = false;
    }
    const ui::DialogSpec spec{.title = "Capture a screenshot", .confirm = "Capture"};
    if (!ui::detail::begin_dialog(DIALOG_ID, spec)) return;

    // What there is to take: the scene through the active viewport's camera,
    // or one of the editor's panes as it is on screen.
    const bool scene = mode_ == Mode::scene;
    const bool can_capture = scene ? viewports.active_camera() != nullptr : !pane_options_.empty();
    if (auto t = ui::PropertyTable("##capture_rows")) {
        ui::row("Capture", [&] {
            int mode = static_cast<int>(mode_);
            if (ui::choice("##mode", mode, MODES).changed) {
                mode_ = static_cast<Mode>(mode);
                if (mode_ == Mode::pane) refresh_pane_list();
            }
        });
        if (scene) {
            ui::row("Resolution", [&] {
                std::vector<const char*> names;
                for (const auto& preset : kResolutionPresets) names.push_back(preset.label);
                (void)ui::choice("##resolution", resolution_preset_, names);
            });
            if (kResolutionPresets[static_cast<size_t>(resolution_preset_)].w == 0) {
                ui::row("Size", "Width and height in pixels.", [&] {
                    glm::ivec2 size{custom_w_, custom_h_};
                    if (ui::ivec2("##size", size, 1.0f, 16, 16384).changed) {
                        custom_w_ = size.x;
                        custom_h_ = size.y;
                    }
                });
            }
            ui::row("Editor overlays", "Draw the grid, the gizmos and the debug views into the picture.",
                    [&] { (void)ui::checkbox("##overlays", include_overlays_); });
            ui::row("Ungraded", "The engine's default look, whatever post process volumes the camera "
                                "is in: a frame to grade in an image editor.",
                    [&] { (void)ui::checkbox("##ungraded", ungraded_); });
        } else if (!pane_options_.empty()) {
            ui::row("Pane", [&] {
                std::vector<const char*> names;
                for (const auto& pane : pane_options_) names.push_back(pane.c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() -
                                        ImGui::GetStyle().ItemSpacing.x);
                (void)ui::choice("##pane", pane_index_, names);
                ImGui::SameLine();
                if (ui::icon_button("##refresh", ui::icon::reload, "Look again for the panes that are open")) {
                    refresh_pane_list();
                }
            });
        }
    }
    if (scene && can_capture) ui::hint("The camera is the active viewport's.");
    if (!can_capture) {
        ui::status(ui::Severity::Warning, scene ? "No 3D viewport is active." : "No panes are open.");
    }

    const auto answer = ui::detail::end_dialog(
        spec, {.can_confirm = can_capture,
               .why_not = scene ? "Needs an active 3D viewport" : "Needs an open pane"});
    if (answer != ui::DialogAnswer::Confirm) return;

    CaptureService::Request req;
    if (scene) {
        req.mode = CaptureService::Mode::scene;
        const auto& preset = kResolutionPresets[static_cast<size_t>(resolution_preset_)];
        req.width = preset.w != 0 ? preset.w : static_cast<uint32_t>(std::max(16, custom_w_));
        req.height = preset.h != 0 ? preset.h : static_cast<uint32_t>(std::max(16, custom_h_));
        req.include_overlays = include_overlays_;
        req.ungraded = ungraded_;
    } else {
        req.mode = CaptureService::Mode::pane;
        req.pane_name = pane_options_[static_cast<size_t>(pane_index_)];
    }
    capture.request(req);
}

} // namespace fjell
