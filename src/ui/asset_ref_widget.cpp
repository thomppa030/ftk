#include "ui/asset_ref_widget.hpp"
#include "ui/content_browser.hpp"
#include "ui/file_browser.hpp"
#include "ui/icon_cache.hpp"
#include "ui/icons_lc.hpp"
#include "core/base_path.hpp"
#include "core/project.hpp"
#include "core/project_manager.hpp"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace fjell {

// ── Browse pending state ────────────────────────────────────────────────
// Only one widget can have an active browse session at a time.

static std::string s_browse_pending_id;      // ImGui ID of the widget waiting
static std::string s_browse_result;          // path delivered by FileBrowser
static bool        s_browse_result_ready{false};

void asset_ref_browse_accept(const std::string& absolute_path,
                             const std::string& project_root) {
    if (s_browse_pending_id.empty()) return;
    if (project_root.empty()) {
        s_browse_result = absolute_path;
    } else {
        std::error_code ec;
        s_browse_result = std::filesystem::relative(
            absolute_path, project_root, ec).string();
        if (ec) s_browse_result = absolute_path;
    }
    s_browse_result_ready = true;
}

// ── Extension matching ──────────────────────────────────────────────────

static bool matches_extension(const std::string& path,
                              std::span<const std::string> extensions) {
    if (extensions.empty()) return true;
    auto ext = std::filesystem::path(path).extension().string();
    return std::ranges::any_of(extensions, [&](const std::string& e) {
        return ext == e;
    });
}

// ── Widget ──────────────────────────────────────────────────────────────

bool asset_ref_field(const AssetRefConfig& config, std::string& path) {
    bool changed = false;

    const Project* project = config.ctx && config.ctx->project_mgr
        ? config.ctx->project_mgr->active() : nullptr;

    // References are stored as ids so they survive the asset being moved, but
    // everything the user sees — the field, the tooltip, the preview — wants
    // the path the id points at.
    auto to_reference = [&](const std::string& picked) {
        return base_path::asset_reference(picked, project);
    };
    auto display_path = [&](const std::string& reference) -> std::string {
        if (!base_path::is_asset_id(reference)) return reference;
        if (!project) return reference;
        auto rel = project->path_of(AssetId::from_string(reference));
        return rel.empty() ? reference : rel;
    };

    ImGui::PushID(config.id);

    // Label (skipped when empty — caller wants single-line layout, e.g. list items)
    const bool has_label = config.label != nullptr && config.label[0] != '\0';
    if (has_label) {
        ImGui::TextUnformatted(config.label);
    }

    auto shown_path = display_path(path);

    // Display field — filename only, full path on hover
    auto display = shown_path.empty()
        ? std::string("(none)")
        : std::filesystem::path(shown_path).filename().string();

    float button_width = ImGui::GetFrameHeight();
    int num_buttons = 0;
    if (config.ctx && config.ctx->content_browser) num_buttons++;
    if (config.ctx && config.ctx->file_browser) num_buttons++;
    if (!path.empty()) num_buttons++;
    float buttons_total = static_cast<float>(num_buttons) * (button_width + ImGui::GetStyle().ItemSpacing.x);

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttons_total);
    ImGui::InputText("##display", display.data(), display.size(),
                     ImGuiInputTextFlags_ReadOnly);

    // Tooltip: full path
    if (ImGui::IsItemHovered() && !shown_path.empty()) {
        ImGui::SetTooltip("%s", shown_path.c_str());
    }

    // Drag-and-drop target
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            std::string dropped(static_cast<const char*>(payload->Data));
            if (matches_extension(dropped, config.extensions)) {
                path = to_reference(dropped);
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Check for pending browse result
    if (s_browse_result_ready && s_browse_pending_id == config.id) {
        if (matches_extension(s_browse_result, config.extensions)) {
            path = to_reference(s_browse_result);
            changed = true;
        }
        s_browse_pending_id.clear();
        s_browse_result.clear();
        s_browse_result_ready = false;
    }

    // Icon buttons — tighter FramePadding so they don't dominate the row.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 4.0f));

    // Use Selected button (pin glyph — "use the pinned/selected asset")
    if (config.ctx && config.ctx->content_browser) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_LC_PIN "##pick", {button_width, button_width})) {
            auto selected = config.ctx->content_browser->selected_asset_path();
            if (!selected.empty() && matches_extension(selected, config.extensions)) {
                path = to_reference(selected);
                changed = true;
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Use selected asset from content browser");
        }
    }

    // Browse button
    if (config.ctx && config.ctx->file_browser) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_LC_FOLDER "##browse", {button_width, button_width})) {
            s_browse_pending_id = config.id;
            s_browse_result_ready = false;
            std::vector<std::string> exts(config.extensions.begin(), config.extensions.end());
            config.ctx->file_browser->open(
                std::string("Select ") + (has_label ? config.label : "asset"),
                FileBrowser::Mode::select_file, exts);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Browse for file");
        }
    }

    // Clear button
    if (!path.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_LC_X "##clear", {button_width, button_width})) {
            path.clear();
            changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Clear");
        }
    }

    ImGui::PopStyleVar();

    // Per-type preview — takes a path, so hand it the resolved one.
    if (config.asset_type && config.asset_type->draw_preview &&
        !shown_path.empty() && config.ctx && config.ctx->resource_manager) {
        config.asset_type->draw_preview(
            shown_path, *config.ctx->resource_manager,
            config.ctx->icons, project,
            ImGui::GetContentRegionAvail().x);
    }

    ImGui::PopID();
    return changed;
}

} // namespace fjell
