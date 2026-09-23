#include "ui/asset_ref_widget.hpp"
#include "ui/asset_picker.hpp"
#include "ui/content_browser.hpp"
#include "ui/file_browser.hpp"
#include "ui/icon_cache.hpp"
#include "ui/icons_lc.hpp"
#include "core/base_path.hpp"
#include "core/project.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/asset_shader_type.hpp"

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

// A pick the field turned down, said under it for a few seconds: the pin
// and browse buttons have nowhere else to say why nothing happened.
static std::string s_refused_id;
static std::string s_refused_text;
static double      s_refused_until{0.0};
constexpr double REFUSAL_SECONDS = 4.0;

// ── Acceptance ──────────────────────────────────────────────────────────

static bool matches_extension(const std::string& path,
                              std::span<const std::string> extensions) {
    if (extensions.empty()) return true;
    auto ext = std::filesystem::path(path).extension().string();
    return std::ranges::any_of(extensions, [&](const std::string& e) {
        return ext == e;
    });
}

/// Why a shader or material cannot go in this slot, or empty when it can.
/// `candidate` is an absolute path, a project-relative one or an id; a file
/// that cannot be found is left for whoever loads it to report.
static std::string shader_type_refusal(const std::string& candidate, const Project* project,
                                       std::span<const ShaderType> accepted) {
    if (accepted.empty() || !carries_shader_type(candidate)) return {};
    std::filesystem::path file = candidate;
    if (!file.is_absolute()) file = base_path::resolve_shared(candidate, project);
    if (file.empty()) return {};
    if (shader_type_accepted(file, project, accepted)) return {};

    const char* kind = file.extension() == ".fjmat" ? "material" : "shader";
    auto name = file.filename().string();
    auto type = asset_shader_type(file, project);
    auto needs = describe_shader_types(accepted);
    if (!type) {
        return name + " is not drawn as any shader type; this slot needs a " + needs + " " + kind;
    }
    return name + " is a " + std::string(shader_type_name(*type)) + " " + kind +
           "; this slot needs " + needs;
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
        auto shown = base_path::asset_display_path(reference, project);
        // An id the database no longer knows: show it, so the field reads as
        // broken rather than empty.
        return shown.empty() ? reference : shown;
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
    // The buffer size counts the terminator std::string keeps behind its
    // characters; ImGui asserts on a buffer with no room for one the moment
    // the field is clicked into.
    ImGui::InputText("##display", display.data(), display.size() + 1,
                     ImGuiInputTextFlags_ReadOnly);

    // Clicking the field picks an asset. The read-only InputText still takes
    // the click to place a caret; the picker opening over it is what the user
    // sees, and the caret goes with the focus when the popup takes it.
    if (ImGui::IsItemClicked()) {
        asset_picker::open(config.id);
    }

    // Tooltip: full path
    if (ImGui::IsItemHovered() && !shown_path.empty()) {
        ImGui::SetTooltip("%s", shown_path.c_str());
    }

    // The picker draws here so it opens under the field rather than wherever
    // the row happens to end.
    if (std::string chosen; asset_picker::draw(config.id, config.extensions, config.shader_types,
                                               project, chosen)) {
        path = to_reference(chosen);
        changed = true;
    }

    // Anything picked by other means than the list: the list only offers
    // what fits, these could bring in anything.
    auto offer = [&](const std::string& candidate) {
        if (!matches_extension(candidate, config.extensions)) return;
        auto refusal = shader_type_refusal(candidate, project, config.shader_types);
        if (!refusal.empty()) {
            s_refused_id = config.id;
            s_refused_text = std::move(refusal);
            s_refused_until = ImGui::GetTime() + REFUSAL_SECONDS;
            return;
        }
        path = to_reference(candidate);
        changed = true;
    };

    // Drag-and-drop target. Taken before delivery so a shader or material of
    // the wrong Type says why while it is still over the field, and the drop
    // is simply not accepted.
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload(
                "ASSET_PATH", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
            std::string dropped(static_cast<const char*>(payload->Data));
            auto refusal = matches_extension(dropped, config.extensions)
                ? shader_type_refusal(dropped, project, config.shader_types)
                : std::string{};
            if (!refusal.empty()) {
                ImGui::SetTooltip("%s", refusal.c_str());
            } else if (payload->IsDelivery()) {
                offer(dropped);
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Check for pending browse result
    if (s_browse_result_ready && s_browse_pending_id == config.id) {
        offer(s_browse_result);
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
            if (!selected.empty()) offer(selected);
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

    // What is wrong with the pick just refused, else with what the field
    // holds: a scene written by hand or before the slot said what it takes.
    std::string problem;
    if (s_refused_id == config.id && ImGui::GetTime() < s_refused_until) {
        problem = s_refused_text;
    } else if (!path.empty()) {
        problem = shader_type_refusal(path, project, config.shader_types);
    }
    if (!problem.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95F, 0.45F, 0.40F, 1.0F));
        ImGui::TextWrapped("%s", problem.c_str());
        ImGui::PopStyleColor();
    }

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

std::vector<std::string> list_project_assets(const Project* project, std::string_view extension) {
    std::vector<std::string> files;
    if (project == nullptr) return files;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(project->root(), ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != extension) continue;
        files.push_back(std::filesystem::relative(it->path(), project->root(), ec).generic_string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace fjell
