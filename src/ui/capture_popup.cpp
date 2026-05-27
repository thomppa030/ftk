#include "ui/capture_popup.hpp"

#include "core/capture_service.hpp"
#include "renderer/camera.hpp"
#include "ui/viewport_manager.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace fjell {

namespace {

struct ResolutionPreset {
    const char* label;
    uint32_t w;
    uint32_t h;
};

constexpr std::array<ResolutionPreset, 5> kResolutionPresets{{
    {"1080p (1920x1080)", 1920, 1080},
    {"1440p (2560x1440)", 2560, 1440},
    {"4K (3840x2160)",    3840, 2160},
    {"8K (7680x4320)",    7680, 4320},
    {"Custom",            0,    0},
}};

constexpr const char* POPUP_TITLE = "Capture Screenshot";

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
        ImGui::OpenPopup(POPUP_TITLE);
        refresh_pane_list();
        want_open_ = false;
    }

    if (!ImGui::BeginPopupModal(POPUP_TITLE, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // Mode selector
    int mode_idx = static_cast<int>(mode_);
    const char* modes[] = {"Scene", "Pane"};
    ImGui::SetNextItemWidth(220.0F);
    if (ImGui::Combo("Mode", &mode_idx, modes, IM_ARRAYSIZE(modes))) {
        mode_ = static_cast<Mode>(mode_idx);
        if (mode_ == Mode::pane) { refresh_pane_list(); }
    }
    ImGui::Separator();

    if (mode_ == Mode::scene) {
        ImGui::SetNextItemWidth(220.0F);
        if (ImGui::BeginCombo("Resolution", kResolutionPresets[resolution_preset_].label)) {
            for (int i = 0; i < static_cast<int>(kResolutionPresets.size()); ++i) {
                bool selected = (i == resolution_preset_);
                if (ImGui::Selectable(kResolutionPresets[i].label, selected)) {
                    resolution_preset_ = i;
                }
                if (selected) { ImGui::SetItemDefaultFocus(); }
            }
            ImGui::EndCombo();
        }
        if (kResolutionPresets[resolution_preset_].w == 0) {
            ImGui::SetNextItemWidth(105.0F);
            ImGui::DragInt("Width##capW", &custom_w_, 1, 16, 16384);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(105.0F);
            ImGui::DragInt("Height##capH", &custom_h_, 1, 16, 16384);
        }
        ImGui::Checkbox("Include editor overlays (grid, gizmos, viz)", &include_overlays_);
        ImGui::TextDisabled("Camera taken from active editor viewport");
    } else {
        if (pane_options_.empty()) {
            ImGui::TextDisabled("No panes visible");
        } else {
            ImGui::SetNextItemWidth(320.0F);
            if (ImGui::BeginCombo("Pane", pane_options_[pane_index_].c_str())) {
                for (int i = 0; i < static_cast<int>(pane_options_.size()); ++i) {
                    bool selected = (i == pane_index_);
                    if (ImGui::Selectable(pane_options_[i].c_str(), selected)) {
                        pane_index_ = i;
                    }
                    if (selected) { ImGui::SetItemDefaultFocus(); }
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::SmallButton("Refresh")) { refresh_pane_list(); }
    }

    ImGui::Separator();
    bool ok = false;
    if (mode_ == Mode::scene) {
        Camera* cam = viewports.active_camera();
        ok = (cam != nullptr);
        if (!ok) { ImGui::TextDisabled("No active 3D viewport"); }
    } else {
        ok = !pane_options_.empty();
    }

    if (!ok) { ImGui::BeginDisabled(); }
    bool capture_clicked = ImGui::Button("Capture", {140, 0});
    if (!ok) { ImGui::EndDisabled(); }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {140, 0})) {
        ImGui::CloseCurrentPopup();
    }

    if (capture_clicked) {
        CaptureService::Request req;
        if (mode_ == Mode::scene) {
            req.mode = CaptureService::Mode::scene;
            auto preset = kResolutionPresets[resolution_preset_];
            req.width = preset.w != 0 ? preset.w : static_cast<uint32_t>(std::max(16, custom_w_));
            req.height = preset.h != 0 ? preset.h : static_cast<uint32_t>(std::max(16, custom_h_));
            req.include_overlays = include_overlays_;
        } else {
            req.mode = CaptureService::Mode::pane;
            req.pane_name = pane_options_[pane_index_];
        }
        capture.request(req);
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace fjell
