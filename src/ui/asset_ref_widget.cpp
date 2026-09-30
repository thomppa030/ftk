#include "ui/asset_ref_widget.hpp"
#include "ui/kit/asset_kind.hpp"
#include "ui/kit/edit_record.hpp"
#include "ui/kit/slot.hpp"
#include "ui/theme.hpp"
#include "ui/asset_picker.hpp"
#include "ui/content_browser.hpp"
#include "ui/icon_cache.hpp"
#include "core/base_path.hpp"
#include "core/project.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/asset_shader_type.hpp"

#include <imgui.h>
#include <stb_image.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace fjell {

// A texture's picture on hover (sheet 19, P3); the field's own is the slot face's.
constexpr float HOVER_THUMB = 128.0f;

static bool is_image(const std::filesystem::path& path) {
    const std::string ext = path.extension().string();
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga";
}

// An image's size in pixels, read from its header once per file.
static ImVec2 image_size(const std::string& path) {
    static std::unordered_map<std::string, ImVec2> sizes;
    if (auto it = sizes.find(path); it != sizes.end()) return it->second;
    int w = 0;
    int h = 0;
    int channels = 0;
    ImVec2 size{0.0f, 0.0f};
    if (stbi_info(path.c_str(), &w, &h, &channels) != 0) {
        size = {static_cast<float>(w), static_cast<float>(h)};
    }
    sizes.emplace(path, size);
    return size;
}

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

    ImGui::PushID(config.id);
    const bool has_value = !path.empty();
    ui::Slot slot("ASSET_PATH", [&](const ImGuiPayload& payload) {
        return drop_refusal(static_cast<const char*>(payload.Data), config.extensions, project,
                            config.shader_types);
    });
    if (slot.clicked()) asset_picker::open(config.id);

    // A texture shows its picture: in the field, and bigger when hovered.
    ImTextureID thumb{};
    std::string resolved;
    if (has_value && !shown_path.empty() && is_image(shown_path) && config.ctx && config.ctx->icons) {
        resolved = base_path::resolve_asset(path, project);
        if (std::filesystem::exists(resolved)) thumb = config.ctx->icons->thumbnail(resolved);
    }
    if (slot.hovered() && !shown_path.empty()) {
        if (thumb) {
            if (ImGui::BeginItemTooltip()) {
                ImGui::Image(thumb, {HOVER_THUMB, HOVER_THUMB});
                ImGui::TextUnformatted(shown_path.c_str());
                if (const ImVec2 px = image_size(resolved); px.x > 0.0f) {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
                    ImGui::Text("%.0f \xc3\x97 %.0f", static_cast<double>(px.x), static_cast<double>(px.y));
                    ImGui::PopStyleColor();
                }
                ImGui::EndTooltip();
            }
        } else {
            ImGui::SetItemTooltip("%s", shown_path.c_str());
        }
    }

    if (const ImGuiPayload* dropped = slot.dropped()) {
        path = to_reference(static_cast<const char*>(dropped->Data));
        changed = true;
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
    const bool missing =
        has_value && (config.failed || shown_path.empty() || !asset_exists(reference, project));
    const std::string name = shown_path.empty()
        ? reference
        : std::filesystem::path(shown_path).filename().string();
    const ui::AssetPresence presence = !has_value ? ui::AssetPresence::empty
                                       : missing  ? ui::AssetPresence::missing
                                                  : ui::AssetPresence::held;
    std::string text;
    ui::SlotFace face = ui::asset_face(
        name, presence, config.extensions.empty() ? std::string_view{} : std::string_view{config.extensions.front()},
        text);
    if (thumb && !missing) face.picture = thumb;

    // What is wrong with what the field holds: a scene written by hand or
    // before the slot said what it takes.
    std::string problem =
        has_value && !missing ? shader_type_refusal(path, project, config.shader_types) : std::string();
    if (!has_value && config.required) problem = ui::REQUIRED_PROBLEM;
    if (slot.finish(face, !has_value, problem)) {
        path.clear();
        changed = true;
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
