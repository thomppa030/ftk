#include "ui/asset_ref_widget.hpp"
#include "ui/kit/asset_kind.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/edit_record.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/icons.hpp"
#include "ui/theme.hpp"
#include "ui/asset_picker.hpp"
#include "ui/content_browser.hpp"
#include "ui/icon_cache.hpp"
#include "core/base_path.hpp"
#include "core/project.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/asset_shader_type.hpp"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace fjell {

// A drop the field turned down, said under it for a few seconds after it
// was let go.
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

// Whether the asset a reference points at is on disk. The file system is
// asked at most once a second per reference: every slot on screen asks every
// frame.
static bool asset_exists(const std::string& reference, const Project* project) {
    struct Seen {
        bool exists;
        double at;
    };
    static std::unordered_map<std::string, Seen> seen;
    const double now = ImGui::GetTime();
    if (auto it = seen.find(reference); it != seen.end() && now - it->second.at < 1.0) {
        return it->second.exists;
    }
    std::error_code ec;
    const std::string file = base_path::resolve_shared(reference, project);
    const bool exists = !file.empty() && std::filesystem::exists(file, ec);
    seen[reference] = {exists, now};
    return exists;
}

// The kinds a slot takes, as words: "material", "mesh or skeleton".
static std::string accepted_nouns(std::span<const std::string> extensions) {
    std::string nouns;
    for (const auto& ext : extensions) {
        const char* noun = ui::asset_kind(ext).noun;
        if (nouns.find(noun) != std::string::npos) continue;
        if (!nouns.empty()) nouns += " or ";
        nouns += noun;
    }
    return nouns.empty() ? std::string("asset") : nouns;
}

// Why a dropped file can't go in this slot, or empty when it can.
static std::string drop_refusal(const std::string& dropped, std::span<const std::string> extensions,
                                const Project* project, std::span<const ShaderType> shader_types) {
    if (!matches_extension(dropped, extensions)) {
        const auto file = std::filesystem::path(dropped);
        return "Takes a " + accepted_nouns(extensions) + ". " + file.filename().string() + " is a " +
               ui::asset_kind(file.extension().string()).noun + ".";
    }
    return shader_type_refusal(dropped, project, shader_types);
}

// ── Widget ──────────────────────────────────────────────────────────────

bool asset_slot(const AssetRefConfig& config, std::string& path) {
    bool changed = false;

    const Project* project = config.ctx && config.ctx->project_mgr
        ? config.ctx->project_mgr->active() : nullptr;

    // References are stored as ids so they survive the asset being moved, but
    // everything the user sees — the field, the tooltip, the preview — wants
    // the path the id points at.
    auto to_reference = [&](const std::string& picked) {
        return base_path::asset_reference(picked, project);
    };
    const std::string shown_path = base_path::asset_display_path(path, project);
    // What the slot is called in an undo step: the asset's file name.
    auto name_of = [&](const std::string& reference) -> std::string {
        if (reference.empty()) return "None";
        const std::string shown = base_path::asset_display_path(reference, project);
        return std::filesystem::path(shown.empty() ? reference : shown).filename().string();
    };
    const std::string name_before = name_of(path);

    // Anything picked by other means than the list: the list only offers
    // what fits, a drop could bring in anything.
    auto offer = [&](const std::string& candidate) {
        auto refusal = drop_refusal(candidate, config.extensions, project, config.shader_types);
        if (!refusal.empty()) {
            s_refused_id = config.id;
            s_refused_text = std::move(refusal);
            s_refused_until = ImGui::GetTime() + REFUSAL_SECONDS;
            return;
        }
        path = to_reference(candidate);
        changed = true;
    };

    ImGui::PushID(config.id);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const float width = ImGui::CalcItemWidth();
    const bool has_value = !path.empty();
    // The clear button is always there, disabled while the slot is empty,
    // so the field keeps its width whether it holds something or not.
    const float clear_w = height + style.ItemInnerSpacing.x;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max{min.x + std::max(width - clear_w, height), min.y + height};

    // The field is one button: clicking anywhere on it opens the picker.
    if (ImGui::InvisibleButton("##slot", {max.x - min.x, height})) {
        asset_picker::open(config.id);
    }
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && !shown_path.empty()) {
        ImGui::SetItemTooltip("%s", shown_path.c_str());
    }

    // A dragged asset is judged while it is still over the field: one that
    // fits lights the field, one that doesn't outlines it red and says why,
    // and is not taken.
    enum class Drop { None, Fits, Refused } drop = Drop::None;
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        if (payload != nullptr && payload->IsDataType("ASSET_PATH")) {
            const std::string dragged(static_cast<const char*>(payload->Data));
            const std::string refusal =
                drop_refusal(dragged, config.extensions, project, config.shader_types);
            if (refusal.empty()) {
                drop = Drop::Fits;
                if (const auto* taken = ImGui::AcceptDragDropPayload(
                        "ASSET_PATH", ImGuiDragDropFlags_AcceptBeforeDelivery
                                          | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
                    taken != nullptr && taken->IsDelivery()) {
                    offer(dragged);
                }
            } else {
                drop = Drop::Refused;
                if (ImGui::BeginTooltip()) {
                    ImGui::PushTextWrapPos(theme::TOOLTIP_WRAP);
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::error());
                    ImGui::TextUnformatted(ui::icon::refused);
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::TextUnformatted(refusal.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    // The picker hangs under the field, so it draws right after it.
    const std::string selected = config.ctx && config.ctx->content_browser
        ? config.ctx->content_browser->selected_asset_path() : std::string{};
    if (std::string chosen; asset_picker::draw(config.id, config.extensions, config.shader_types,
                                               project, selected, chosen)) {
        path = to_reference(chosen);
        changed = true;
    }

    // What the field shows: the asset's kind and name, "None" when empty,
    // and a missing asset by name in the error colour, never as empty.
    const std::string& reference = path;
    const bool missing = has_value && (shown_path.empty() || !asset_exists(reference, project));
    const std::string name = shown_path.empty()
        ? reference
        : std::filesystem::path(shown_path).filename().string();
    const auto& kind = ui::asset_kind(std::filesystem::path(shown_path.empty() ? reference : shown_path)
                                          .extension().string());
    const char* glyph = missing ? ui::icon::warning : kind.icon;
    ImVec4 icon_colour = theme::category(kind.category);
    ImVec4 text_colour = theme::text();
    std::string text = name;
    if (!has_value) {
        glyph = config.extensions.empty() ? ui::icon::file
                                          : ui::asset_kind(config.extensions.front()).icon;
        icon_colour = theme::text_disabled();
        text_colour = theme::text_secondary();
        text = "None";
    } else if (missing) {
        icon_colour = theme::error();
        text_colour = theme::error();
        text = "missing: " + name;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = style.FrameRounding;
    dl->AddRectFilled(min, max, ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
                      rounding);
    if (drop == Drop::Fits) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(theme::drop_fits()), rounding);
        dl->AddRect(min, max, ImGui::GetColorU32(theme::accent()), rounding);
    } else if (drop == Drop::Refused) {
        dl->AddRect(min, max, ImGui::GetColorU32(theme::error()), rounding);
    }
    const float text_y = min.y + style.FramePadding.y;
    float x = min.x + style.FramePadding.x;
    dl->AddText({x, text_y}, ImGui::GetColorU32(icon_colour), glyph);
    x += ImGui::CalcTextSize(glyph).x + theme::GAP_S + theme::GAP_XS;
    const float chevron = height;
    dl->PushClipRect({x, min.y}, {max.x - chevron, max.y}, true);
    dl->AddText({x, text_y}, ImGui::GetColorU32(text_colour), text.c_str());
    dl->PopClipRect();
    const float chevron_w = ImGui::CalcTextSize(ui::icon::dropdown).x;
    dl->AddText({max.x - chevron + (chevron - chevron_w) * 0.5f, text_y},
                ImGui::GetColorU32(theme::text_secondary()), ui::icon::dropdown);

    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    ImGui::BeginDisabled(!has_value);
    if (ui::icon_button("##clear", ui::icon::clear, has_value ? "Clear" : "Nothing to clear",
                        ui::ButtonKind::GhostDanger)) {
        path.clear();
        changed = true;
    }
    ImGui::EndDisabled();

    // What is wrong with the pick just refused, else with what the field
    // holds: a scene written by hand or before the slot said what it takes.
    std::string problem;
    if (s_refused_id == config.id && ImGui::GetTime() < s_refused_until) {
        problem = s_refused_text;
    } else if (has_value && !missing) {
        problem = shader_type_refusal(path, project, config.shader_types);
    }
    if (!problem.empty()) {
        ui::status(ui::Severity::Error, problem.c_str());
    }

    // Per-type preview — takes a path, so hand it the resolved one.
    if (config.asset_type && config.asset_type->draw_preview && !shown_path.empty() && !missing &&
        config.ctx && config.ctx->resource_manager) {
        config.asset_type->draw_preview(shown_path, *config.ctx->resource_manager, config.ctx->icons,
                                        project, ImGui::GetContentRegionAvail().x);
    }

    ImGui::PopID();
    if (changed) {
        ui::detail::track_field(ImGui::GetID(config.id), name_before, name_of(path), false, {true, true});
    }
    return changed;
}

bool asset_ref_field(const AssetRefConfig& config, std::string& path) {
    if (config.label != nullptr && config.label[0] != '\0') {
        ImGui::TextUnformatted(config.label);
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    return asset_slot(config, path);
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
