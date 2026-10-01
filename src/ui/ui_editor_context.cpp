#include "ui/ui_editor_context.hpp"
#include "ui/ui_preview_data.hpp"
#include "scene/component_meta.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/ui_data_bind.hpp"
#include "ui/game_ui/fjss_loader.hpp"
#include "ui/game_ui/fjui_serializer.hpp"
#include "ui/game_ui/ui_style_commands.hpp"
#include "ui/game_ui/ui_style_properties.hpp"
#include "ui/game_ui/widget_asset.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "ftk/ui/kit/canvas.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/color_field.hpp"
#include "ftk/ui/kit/dialog.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/list_editor.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/text_field.hpp"
#include "ftk/ui/kit/tree.hpp"
#include "ui/material_params_widget.hpp"
#include "scripting/script_manager.hpp"
#include "renderer/bindless.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/texture.hpp"
#include "core/base_path.hpp"
#include "core/clock.hpp"
#include "core/input.hpp"
#include "ftk/base/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "ftk/app/icon_cache.hpp"
#include "ftk/app/imgui_layer.hpp"

#include <imgui.h>

#include <cstring>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <optional>
#include <span>
#include <tuple>
#include <fstream>

namespace fjell {

namespace fs = std::filesystem;

UiEditorContext::UiEditorContext(UiEditorDeps deps)
    : deps_{deps} {}

UiEditorContext::~UiEditorContext() = default;

ftk::EditorContext::DockPreset UiEditorContext::dock_preset() const {
    return {.tree = {"Elements"}, .preview = {"Preview"}, .under_preview = {"Action log"}, .inspector = {"Properties", "Data"}};
}

void UiEditorContext::init() {
    // Preview GameUi is initialized lazily when a .fjui file is opened

    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());

    // What the preview built from the game module goes before the module
    // does; the preview opens again once the new one is in.
    if (deps_.script_manager != nullptr) {
        library_unloading_ = deps_.script_manager->on_unloading.bind([this] {
            if (preview_data_) preview_data_->release();
            reopen_ = true;
        });
    }
}

void UiEditorContext::shutdown() {
    // Clear preview callback before destroying resources
    deps_.vulkan_context->set_ui_preview_callback(nullptr);
    library_unloading_.disconnect();
    preview_data_.reset();

    // Clear the UiContext's action delegates before disconnecting connections.
    if (preview_ui_) {
        preview_ui_->context().clear();
    }
    selected_node_ = nullptr;

    deps_.vulkan_context->wait_idle();
    for (auto& pass : offscreen_) {
        pass.reset();
    }
    if (preview_ui_) {
        preview_ui_->destroy();
        preview_ui_.reset();
    }
}

std::string UiEditorContext::markup_text() const {
    if (!preview_ui_ || !preview_ui_->root()) return {};
    return is_widget_file_ ? widget_asset_text(loaded_widget_, preview_ui_->root())
                           : FjuiSerializer::serialize(*preview_ui_->root());
}

std::string UiEditorContext::document() const {
    if (!preview_ui_ || !preview_ui_->root() || fjui_path_.empty()) return {};
    // The markup and its sheet, as the two files hold them.
    std::string text = markup_text();
    if (!fjss_path_.empty()) {
        text += '\0';
        text += FjssSerializer::serialize(preview_ui_->stylesheet(), fjss_path_);
    }
    return text;
}

ftk::Result<> UiEditorContext::write() {
    if (!preview_ui_ || !preview_ui_->root() || fjui_path_.empty()) return ftk::make_error("no UI is open");
    auto markup_path = base_path::resolve_asset(fjui_path_, deps_.project_mgr->active());

    // The watcher would otherwise see these writes and reopen the files we
    // are saving, throwing away selection and undo history for nothing.
    preview_ui_->expect_write(markup_path);
    preview_ui_->expect_write(fjss_path_);

    {
        std::ofstream out(markup_path);
        if (!out) return ftk::make_error("cannot write " + markup_path);
        out << markup_text();
    }
    if (!fjss_path_.empty()) {
        std::ofstream out(fjss_path_);
        if (!out) return ftk::make_error("cannot write " + fjss_path_);
        out << FjssSerializer::serialize(preview_ui_->stylesheet(), fjss_path_);
    }
    FTK_CORE_INFO("UI editor: saved {} and {}", markup_path, fjss_path_);
    check_widget_files();
    return {};
}

ftk::Result<> UiEditorContext::restore(const std::string& text) {
    if (!preview_ui_ || !preview_ui_->root()) return ftk::make_error("no UI is open");
    // Only the stylesheet is edited here; the markup comes back as it is.
    const auto split = text.find('\0');
    if (text.substr(0, split) != markup_text()) {
        return ftk::make_error("the markup changed, and this editor only puts a stylesheet back");
    }
    if (split != std::string::npos && !fjss_path_.empty()) {
        Stylesheet sheet = FjssParser::parse(std::string_view(text).substr(split + 1), fjss_path_);
        resolve_imports(sheet, fs::path(fjss_path_).parent_path().string());
        preview_ui_->stylesheet() = std::move(sheet);
        preview_ui_->mark_dirty();
    }
    return {};
}

std::string UiEditorContext::saved_files() const {
    std::string files = fs::path(fjui_path_).filename().string();
    if (!fjss_path_.empty()) files += " and " + fs::path(fjss_path_).filename().string();
    return files;
}

void UiEditorContext::draw(float dt) {
    elapsed_time_ += dt;

    // A source file changed on disk: rebuild the preview from the files.
    // Inspector edits live only in the in-memory stylesheet and do not
    // survive this, and the history's commands point into the old tree.
    // A source file changed on disk, or the game module was replaced:
    // rebuild the preview from the files.
    if (preview_ui_ && (preview_ui_->consume_reload_request() || reopen_)) {
        reopen_ = false;
        command_history().clear();
        open_ui(fjui_path_);
    }
    // The scene moves on its own while it plays; the preview reads it now.
    if (preview_ui_ && preview_data_) preview_data_->bind(*preview_ui_);
    // The game was built again: what the widgets name may have changed.
    if (deps_.widget_checks != nullptr && deps_.widget_checks->revision() != widget_checks_revision_) {
        check_widget_files();
    }

    draw_preview(dt);
    draw_element_tree();
    draw_inspector();
    draw_data();
    draw_action_log();

    draw_shared_panels(dt);
}

bool UiEditorContext::handle_shortcuts() {
    if (ImGui::IsAnyItemActive()) return false;

    // Escape: deselect
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        selected_node_ = nullptr;
        return true;
    }

    return false;
}

void UiEditorContext::apply_resize(VulkanContext& /*vk*/) {
    // No offscreen passes to resize
}

// ── Open from asset ──────────────────────────────────────────────────────

void UiEditorContext::open_ui(const std::string& path) {
    auto abs_path = base_path::resolve_asset(path, deps_.project_mgr->active());

    // Determine if this is a .fjwidget (markup format) or a legacy .fjui
    bool is_widget_file = path.ends_with(".fjwidget");
    if (is_widget_file) {
        auto result = load_widget_asset(abs_path);
        if (result) {
            loaded_widget_ = std::move(*result);
            fjui_path_ = path; // track the .fjwidget path as the asset path
            if (!loaded_widget_.fjss_path.empty()) {
                // Resolve .fjss relative to the widget file's directory
                fjss_path_ = (fs::path(abs_path).parent_path() / loaded_widget_.fjss_path).string();
            } else {
                auto fjss = fs::path(abs_path);
                fjss.replace_extension(".fjss");
                fjss_path_ = fjss.string();
            }
        } else {
            FTK_CORE_WARN("UiEditor: {}", result.error());
            fjui_path_ = path;
            fjss_path_.clear();
        }
    } else {
        fjui_path_ = path;
        // Derive .fjss path: same name, .fjss extension
        auto fjss = fs::path(abs_path);
        fjss.replace_extension(".fjss");
        fjss_path_ = fjss.string();
    }

    // Set tab name to filename
    name_ = fs::path(path).stem().string();

    // Initialize preview GameUi if needed
    if (!preview_ui_) {
        preview_ui_ = std::make_unique<GameUi>();
        preview_data_ = std::make_unique<UiPreviewData>(*deps_.script_manager, deps_.scene);
        // A repeat stamped or removed brings or takes the Models of the
        // widgets in it; set before the first load, which can stamp some.
        preview_ui_->set_repeat_hooks(
            [this](UiNode& repeat) {
                preview_ui_->models().add_widgets(repeat);
                preview_ui_->models().bind();
            },
            [this](UiNode& repeat) { preview_ui_->models().remove_widgets(repeat); });
        if (deps_.script_manager != nullptr) {
            preview_ui_->pipes().set_game_pipes(&deps_.script_manager->reflected_types());
        }
        if (deps_.project_mgr && deps_.project_mgr->active()) {
            const auto& ui = deps_.project_mgr->active()->config().ui;
            preview_width_ = static_cast<float>(ui.design_width);
            preview_height_ = static_cast<float>(ui.design_height);
        }
        // The offscreen color format, not the swapchain's: the preview
        // renders to its own pass.
        deps_.vulkan_context->init_game_ui(*preview_ui_, OffscreenPass::COLOR_FORMAT);
        preview_ui_->set_resource_manager(deps_.resource_manager);
        // Without this, resolve_textures()'s .fjmat fallback path resolves
        // relative to the engine's own build/ directory instead of the
        // active project — any material referenced by a project-relative
        // path (not a name-registered material like "default") silently
        // fails to resolve in the editor preview.
        if (deps_.project_mgr) {
            preview_ui_->set_project(deps_.project_mgr->active());
        }

        // Create offscreen passes for pixel-perfect preview (one per frame in flight)
        offscreen_width_ = 800;
        offscreen_height_ = 600;
        for (uint32_t i = 0; i < PREVIEW_FRAMES; ++i) {
            offscreen_[i] = std::make_unique<OffscreenPass>(deps_.vulkan_context->gpu_device(), 800, 600,
                                                            ftk::gpu::Samples::x1, OffscreenPass::Contents::color);
        }

        // Register preview callback — renders to the current frame's offscreen pass
        deps_.vulkan_context->set_ui_preview_callback(
            [this](uint32_t frame_index) {
                if (!preview_ui_ || !preview_ui_->loaded()) return;
                uint32_t slot = frame_index % PREVIEW_FRAMES;
                auto& target = offscreen_[slot];
                if (!target) return;
                deps_.vulkan_context->record_game_ui_offscreen(
                    *preview_ui_, *target, frame_index);
                latest_rendered_offscreen_.store(slot, std::memory_order_release);
            });
    }

    // What the preview reads goes in before the load: material="{prop}"
    // attributes are resolved once, while loading, against whatever the
    // context holds then. Models are found once the tree is there.
    preview_ui_->models().clear();
    const WidgetAsset no_widget;
    preview_data_->set_widget(is_widget_file ? loaded_widget_ : no_widget, abs_path, deps_.project_mgr->active());
    preview_data_->fill(preview_ui_->context());

    // Load the UI — either from inline markup tree or from .fjui file
    if (is_widget_file && loaded_widget_.markup_root) {
        auto tree_clone = UiDataBind::clone_node(*loaded_widget_.markup_root, nullptr);
        auto base_dir = fs::path(abs_path).parent_path().string();
        preview_ui_->load_from_tree(std::move(tree_clone), fjss_path_, base_dir, abs_path);
    } else {
        preview_ui_->load(abs_path, fjss_path_);
    }

    preview_data_->attach(*preview_ui_);

    // The preview runs no controller, so it answers every action at its
    // root: it logs it, with the arguments a controller would be given.
    action_log_.clear();
    preview_ui_->context().set_action_handler([this](std::string_view action, std::span<const UiValue> args) {
        std::string call(action);
        if (!args.empty()) {
            call += '(';
            for (std::size_t i = 0; i < args.size(); ++i) call += (i > 0 ? ", " : "") + args[i].text();
            call += ')';
        }
        action_log_.push_back({std::move(call), elapsed_time_});
        return true;
    });

    // A widget's file keeps its header when saved.
    is_widget_file_ = is_widget_file ||
                      fjui_path_.find("/widgets/") != std::string::npos ||
                      fjui_path_.find("\\widgets\\") != std::string::npos;

    selected_node_ = nullptr;
    check_widget_files();
    document_opened();
    FTK_CORE_INFO("UI editor opened: {}", fjui_path_);
}

// ── Preview Panel ────────────────────────────────────────────────────────

void UiEditorContext::draw_preview(float dt) {
    auto panel = ftk::ui::Panel(ftk::ui::icon::preview, ctx_title("Preview").c_str());
    if (!panel) return;
    if (!preview_ui_ || !preview_ui_->loaded()) {
        ftk::ui::empty_state(ftk::ui::icon::ui_layout, "Nothing to preview", "The markup couldn't be loaded; the console says why");
        return;
    }

    draw_preview_toolbar();

    // Resolution changed — resize all offscreen passes
    uint32_t new_w = static_cast<uint32_t>(preview_width_);
    uint32_t new_h = static_cast<uint32_t>(preview_height_);
    if ((new_w != offscreen_width_ || new_h != offscreen_height_) && new_w > 0 && new_h > 0) {
        for (auto& pass : offscreen_) {
            if (pass) pass->resize(new_w, new_h);
        }
        offscreen_width_ = new_w;
        offscreen_height_ = new_h;
        preview_ui_->mark_dirty();
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 1.0f || avail.y < 1.0f) return;

    // Update the preview UI (layout, data bind, input in play mode)
    const Input* input_ptr = play_mode_ ? deps_.input : nullptr;
    // Must match the frame_index record_game_ui_offscreen() will use
    // for this same GameUi later this frame (via ui_preview_callback_)
    // — a fixed index here would only ever flatten fresh quad data
    // into one of the PREVIEW_FRAMES SSBO slots, leaving the other
    // slots (and the offscreen images tied to them) permanently
    // stale, which reads as "edits don't show up in the preview."
    UiDesignFrame design_frame{};
    if (design_scale_ && deps_.project_mgr && deps_.project_mgr->active()) {
        const auto& ui = deps_.project_mgr->active()->config().ui;
        design_frame = {static_cast<float>(ui.design_width), static_cast<float>(ui.design_height)};
    }
    preview_ui_->set_design_frame(design_frame);
    preview_ui_->update(preview_width_, preview_height_,
                        deps_.vulkan_context->current_frame_index(), dt, input_ptr);

    // Scale to fit available area
    float scale_x = avail.x / preview_width_;
    float scale_y = avail.y / preview_height_;
    float scale = std::min(scale_x, scale_y);
    // Overlays and picking work on layout rects, which are in design-frame
    // pixels; the preview image is in preview pixels. Two scales apart.
    float layout_scale = scale * preview_ui_->frame_scale();

    float scaled_w = preview_width_ * scale;
    float scaled_h = preview_height_ * scale;

    // Center in available area
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    float offset_x = (avail.x - scaled_w) * 0.5f;
    float offset_y = (avail.y - scaled_h) * 0.5f;
    ImVec2 origin = {cursor.x + offset_x, cursor.y + offset_y};

    // Display offscreen render as ImGui image — the slot the
    // ui_preview_callback_ most recently finished rendering into
    // (latest_rendered_offscreen_), not a fixed index. Each slot's
    // pixels only update once every PREVIEW_FRAMES real frames (on
    // the frame its own index comes up in current_frame_'s cycle),
    // so always displaying slot 0 meant up to 2 of every 3 renders
    // — including every render triggered by a live material-param
    // edit that happened to land while slot 0 wasn't due — were
    // silently discarded in favor of stale content from 1-2 frames
    // earlier, with no guarantee that stale content ever became
    // current again while continuously re-editing.
    {
        ImTextureID tex{};
        uint32_t latest = latest_rendered_offscreen_.load(std::memory_order_acquire);
        if (latest < PREVIEW_FRAMES && offscreen_[latest]) {
            tex = ftk::imgui_texture(offscreen_[latest]->color_texture());
        }
        if (!tex) {
            // Fall back to any valid texture before the first real
            // render has completed (e.g. the very first frame).
            for (uint32_t i = 0; i < PREVIEW_FRAMES; ++i) {
                if (offscreen_[i]) {
                    tex = ftk::imgui_texture(offscreen_[i]->color_texture());
                    if (tex) break;
                }
            }
        }
        if (tex) {
            ImGui::SetCursorScreenPos(origin);
            ImGui::Image(tex, {scaled_w, scaled_h});
        }
    }

    // Overlay: layout debug (Edit mode only, drawn via ImGui DrawList on top)
    auto* root = preview_ui_->root();
    if (!play_mode_ && show_layout_overlay_ && root) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const bool over = ImGui::IsWindowHovered() && mouse.x >= origin.x && mouse.y >= origin.y &&
                          mouse.x <= origin.x + scaled_w && mouse.y <= origin.y + scaled_h;
        UiNode* hovered = over ? node_at(root, mouse, origin, layout_scale) : nullptr;
        draw_layout_overlay(root, hovered, ImGui::GetWindowDrawList(), origin, layout_scale);
    }

    // Invisible button for interaction (placed over the image)
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##preview_area", {scaled_w, scaled_h});

    if (play_mode_) {
        // Play mode: forward mouse events to GameUi for interaction
        if (root) {
            process_play_input(root, origin, layout_scale);
        }
    } else if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // Edit mode: click-to-select elements
        selected_node_ = root ? node_at(root, ImGui::GetMousePos(), origin, layout_scale) : nullptr;
    }
}

void UiEditorContext::draw_preview_toolbar() {
    // Left: what a click in the preview does, and the layout overlay.
    static constexpr const char* MODES[] = {"Edit", "Play"};
    int mode = play_mode_ ? 1 : 0;
    ImGui::SetNextItemWidth(120.0f);
    if (ftk::ui::choice("##mode", mode, MODES).changed) play_mode_ = mode == 1;
    ImGui::SetItemTooltip("Edit selects elements with a click; Play hands the mouse to the UI");
    ImGui::SameLine();
    ImGui::BeginDisabled(play_mode_);
    if (ftk::ui::toggle_button("##layout", ftk::ui::icon::layout, show_layout_overlay_, "Layout overlay")) {
        show_layout_overlay_ = !show_layout_overlay_;
    }
    ImGui::EndDisabled();

    // Right: the size the UI is laid out at. Aspect presets keep the height
    // and derive the width: with design scaling the aspect is what changes
    // how a HUD holds up, and a portrait or ultrawide window is one click
    // away.
    struct AspectPreset {
        const char* label;
        float ratio;
    };
    static constexpr AspectPreset ASPECTS[] = {
        {"16:9", 16.0f / 9.0f}, {"16:10", 16.0f / 10.0f}, {"21:9", 21.0f / 9.0f},
        {"32:9", 32.0f / 9.0f}, {"4:3", 4.0f / 3.0f}, {"3:2", 3.0f / 2.0f},
        {"9:16 portrait", 9.0f / 16.0f},
    };
    static constexpr const char* ASPECT_NAMES[] = {"16:9", "16:10", "21:9", "32:9", "4:3", "3:2", "9:16 portrait",
                                                   "Custom"};
    constexpr int CUSTOM = static_cast<int>(std::size(ASPECTS));
    int aspect = CUSTOM;
    for (int i = 0; i < CUSTOM; ++i) {
        if (std::abs(preview_width_ / preview_height_ - ASPECTS[i].ratio) < 0.01f) aspect = i;
    }

    const Project* project = deps_.project_mgr != nullptr ? deps_.project_mgr->active() : nullptr;
    const ImGuiStyle& style = ImGui::GetStyle();
    constexpr float ASPECT_W = 120.0f;
    constexpr float SIZE_W = 84.0f;
    const float times_w = ImGui::CalcTextSize("\xc3\x97").x;
    const float button = ImGui::GetFrameHeight();
    const std::string design_label = std::string(ftk::ui::icon::revert) + "  Design size";
    const float design_w = project != nullptr ? ImGui::CalcTextSize(design_label.c_str()).x + style.FramePadding.x * 2.0f : 0.0f;
    const float gap = style.ItemSpacing.x;
    const float right = ASPECT_W + SIZE_W * 2.0f + times_w + button + (project != nullptr ? design_w + gap : 0.0f) + gap * 5.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(ImGui::GetContentRegionAvail().x - right, 0.0f));

    ImGui::SetNextItemWidth(ASPECT_W);
    if (ftk::ui::choice("##aspect", aspect, ASPECT_NAMES).changed && aspect < CUSTOM) {
        preview_width_ = std::round(preview_height_ * ASPECTS[aspect].ratio);
    }
    ImGui::SetItemTooltip("Aspect ratio at the current height");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(SIZE_W);
    (void)ftk::ui::drag("##width", preview_width_,
                   {.speed = 1.0f, .min = 100.0f, .max = 3840.0f, .unit = ftk::ui::Unit::Pixels, .format = "%.0f"});
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("\xc3\x97");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(SIZE_W);
    (void)ftk::ui::drag("##height", preview_height_,
                   {.speed = 1.0f, .min = 100.0f, .max = 2160.0f, .unit = ftk::ui::Unit::Pixels, .format = "%.0f"});
    ImGui::SameLine();
    if (ftk::ui::toggle_button("##design_scale", ftk::ui::icon::design_scale, design_scale_,
                          "Design scale: fit the project's design frame to the preview, as a screen-space "
                          "placement does in game. Off for world-space widgets")) {
        design_scale_ = !design_scale_;
    }
    if (project != nullptr) {
        ImGui::SameLine();
        const auto& ui = project->config().ui;
        if (ftk::ui::action(ftk::ui::icon::revert, "Design size", ftk::ui::ButtonKind::Ghost)) {
            preview_width_ = static_cast<float>(ui.design_width);
            preview_height_ = static_cast<float>(ui.design_height);
        }
        ImGui::SetItemTooltip("Back to the project's design resolution, %u \xc3\x97 %u", ui.design_width,
                              ui.design_height);
    }
}

// ── Element Tree Panel ───────────────────────────────────────────────────

void UiEditorContext::draw_element_tree() {
    auto panel = ftk::ui::Panel(ftk::ui::icon::hierarchy, ctx_title("Elements").c_str());
    if (!panel) return;
    if (!preview_ui_ || !preview_ui_->loaded()) {
        ftk::ui::empty_state(ftk::ui::icon::ui_layout, "No elements", "The markup couldn't be loaded; the console says why");
        return;
    }
    if (auto* root = preview_ui_->root()) {
        if (auto tree = ftk::ui::Tree("##elements")) draw_node_tree(root, 0);
    }
}

void UiEditorContext::draw_node_tree(UiNode* node, int depth) {
    // Foreach templates are hidden; only their copies are shown.
    if (node->is_foreach_template) return;

    // What kind of node it is: <tag> .class #id, a widget, a foreach copy,
    // or text.
    std::string label;
    const char* icon = ftk::ui::icon::ui_element;
    std::optional<ftk::theme::Category> category;
    if (node->type == UiNodeType::text) {
        label = "\"" + node->text_content + "\"";
        icon = ftk::ui::icon::ui_text;
    } else if (node->is_widget) {
        label = "<" + node->widget_tag + "/>";
        if (!node->id.empty()) label += " #" + node->id;
        icon = ftk::ui::icon::ui_widget;
        category = ftk::theme::Category::Ui;
    } else if (node->foreach_item_index >= 0) {
        label = "<" + node->tag + "> [" + std::to_string(node->foreach_item_index) + "]";
        icon = ftk::ui::icon::ui_repeat;
        category = ftk::theme::Category::Ui;
    } else {
        label = "<" + (node->tag.empty() ? "root" : node->tag) + ">";
        if (!node->id.empty()) label += " #" + node->id;
        for (const auto& cls : node->classes) {
            // Skip auto-generated scope classes
            if (cls.starts_with("__w_")) continue;
            label += " ." + cls;
        }
    }

    const std::string id = std::to_string(reinterpret_cast<std::uintptr_t>(node));
    const auto row = ftk::ui::tree_row({
        .id = id.c_str(),
        .name = label,
        .depth = depth,
        .has_children = node->type != UiNodeType::text && !node->children.empty(),
        .icon = icon,
        .category = category,
        .selection = node == selected_node_ ? ftk::ui::RowSelection::Primary : ftk::ui::RowSelection::None,
        // Not drawn right now: an if="" that is false, or display: none.
        .dimmed = node->hidden_by_if || node->style.display == UiDisplay::none,
    });
    if (row.clicked) selected_node_ = node;
    if (row.open) {
        for (auto& child : node->children) draw_node_tree(child.get(), depth + 1);
    }
}

// ── Inspector Panel ──────────────────────────────────────────────────────

void UiEditorContext::check_widget_files() {
    widget_problems_.clear();
    if (deps_.widget_checks == nullptr) return;
    widget_checks_revision_ = deps_.widget_checks->revision();
    if (!fjui_path_.ends_with(".fjwidget")) return;

    // The open file, then every widget reached through its placements.
    std::vector<std::string> files{base_path::resolve_asset(fjui_path_, deps_.project_mgr->active())};
    for (std::size_t i = 0; i < files.size(); ++i) {
        WidgetCheck result = deps_.widget_checks->check(files[i]);
        std::ranges::move(result.problems, std::back_inserter(widget_problems_));
        for (auto& placed : result.placed) {
            if (std::ranges::find(files, placed) == files.end()) files.push_back(std::move(placed));
        }
    }
}

void UiEditorContext::draw_problems() {
    if (!preview_ui_) return;
    const auto& diagnostics = preview_ui_->stylesheet().diagnostics;
    if (diagnostics.empty() && widget_problems_.empty()) return;

    ftk::ui::section("Problems", ftk::ui::icon::warning);
    auto line = [](const std::string& file, uint32_t row, uint32_t column, const std::string& message) {
        return fs::path(file).filename().string() + ":" + std::to_string(row) + ":" + std::to_string(column) +
               "  " + message;
    };
    for (const auto& d : diagnostics) {
        const std::string text = line(d.source, d.line, d.column, d.message);
        ftk::ui::status(d.severity == FjssSeverity::error ? ftk::ui::Severity::Error : ftk::ui::Severity::Warning, text.c_str());
    }
    for (const auto& p : widget_problems_) {
        ftk::ui::status(ftk::ui::Severity::Warning, line(p.file, p.line, p.column, p.message).c_str());
    }
}

void UiEditorContext::draw_inspector() {
    auto panel = ftk::ui::Panel(ftk::ui::icon::properties, ctx_title("Properties").c_str());
    if (!panel) return;
    // Problems show up next to the UI they broke, even with nothing
    // selected: a typo in a hot-reloaded file should be the first thing
    // visible, not something to dig out of the console.
    draw_problems();

    if (!selected_node_) {
        ftk::ui::empty_state(ftk::ui::icon::nothing_selected, "Nothing selected", "Select an element in the tree or the preview");
        return;
    }
    auto* node = selected_node_;

    ftk::ui::section("Element", ftk::ui::icon::ui_element);
    if (auto t = ftk::ui::PropertyTable("##element")) {
        const std::string tag = node->tag.empty() ? "root" : node->tag;
        ftk::ui::row("Tag", [&] { ftk::ui::readout(tag.c_str()); });
        if (!node->id.empty()) ftk::ui::row("ID", [&] { ftk::ui::readout(node->id.c_str()); });
        std::string classes;
        for (const auto& c : node->classes) {
            if (c.starts_with("__w_")) continue;  // scope classes
            if (!classes.empty()) classes += " ";
            classes += "." + c;
        }
        if (!classes.empty()) ftk::ui::row("Classes", [&] { ftk::ui::readout(classes.c_str()); });
        if (node->type == UiNodeType::text) ftk::ui::row("Text", [&] { ftk::ui::readout(node->text_content.c_str()); });
        if (!node->text_template.empty()) ftk::ui::row("Template", [&] { ftk::ui::readout(node->text_template.c_str()); });
    }
    if (node->hidden_by_if) ftk::ui::status(ftk::ui::Severity::Warning, "Hidden: its if= is false");

    if (node->is_widget) {
        ftk::ui::section("Widget", ftk::ui::icon::ui_widget);
        if (auto t = ftk::ui::PropertyTable("##widget")) {
            const std::string tag = "<" + node->widget_tag + "/>";
            ftk::ui::row("Tag", [&] { ftk::ui::readout(tag.c_str()); });
            for (const auto& [inner, outer] : node->prop_map) {
                ftk::ui::row(inner.c_str(), "A prop of the widget and what this placement passes it.",
                        [&] { ftk::ui::readout(outer.c_str()); });
            }
            for (const auto& [event, action] : node->event_map) {
                const std::string label = "on:" + event;
                ftk::ui::row(label.c_str(), [&] { ftk::ui::readout(action.c_str()); });
            }
            if (deps_.script_manager && deps_.script_manager->is_loaded()) {
                auto* ui_type = deps_.script_manager->find_ui_type_by_tag(node->widget_tag);
                ftk::ui::row("Controller", [&] { ftk::ui::readout(ui_type != nullptr ? ui_type->name.c_str() : "None registered"); });
                if (ui_type != nullptr) {
                    for (const auto& f : ui_type->fields) {
                        const char* type_name = "?";
                        switch (f.type) {
                            case FJELL_FIELD_FLOAT: type_name = "float"; break;
                            case FJELL_FIELD_INT: type_name = "int"; break;
                            case FJELL_FIELD_BOOL: type_name = "bool"; break;
                            case FJELL_FIELD_STRING:
                            case FJELL_FIELD_STD_STRING: type_name = "string"; break;
                            case FJELL_FIELD_VEC2: type_name = "vec2"; break;
                            case FJELL_FIELD_VEC3: type_name = "vec3"; break;
                            case FJELL_FIELD_VEC4: type_name = "vec4"; break;
                            case FJELL_FIELD_STRUCT:
                                type_name = f.struct_type != nullptr ? f.struct_type->name.c_str() : "struct";
                                break;
                            case FJELL_FIELD_VECTOR: type_name = "list"; break;
                            default: break;
                        }
                        ftk::ui::row(f.name.c_str(), "A field the controller publishes as {key}.",
                                [&] { ftk::ui::readout(type_name); });
                    }
                }
            }
        }
    }

    if (node->foreach_item_index >= 0) {
        ftk::ui::section("Foreach", ftk::ui::icon::ui_repeat);
        if (auto t = ftk::ui::PropertyTable("##foreach")) {
            const std::string item = std::to_string(node->foreach_item_index + 1);
            ftk::ui::row("Item", [&] { ftk::ui::readout(item.c_str()); });
        }
    }

    bool has_attributes = false;
    for (const auto& [key, value] : node->attributes) has_attributes |= !key.starts_with("_");
    if (has_attributes) {
        ftk::ui::section("Attributes", ftk::ui::icon::ui_element);
        if (auto t = ftk::ui::PropertyTable("##attributes")) {
            for (const auto& [key, value] : node->attributes) {
                if (key.starts_with("_")) continue;  // internal attributes
                ftk::ui::row(key.c_str(), [&] { ftk::ui::readout(value.c_str()); });
            }
        }
    }

    ftk::ui::section("State", ftk::ui::icon::info);
    if (auto t = ftk::ui::PropertyTable("##state")) {
        ftk::ui::row("Hovered", [&] { ftk::ui::readout(node->is_hovered() ? "Yes" : "No"); });
        ftk::ui::row("Active", [&] { ftk::ui::readout(node->is_active() ? "Yes" : "No"); });
        ftk::ui::row("Focused", [&] { ftk::ui::readout(node->is_focused() ? "Yes" : "No"); });
    }

    ftk::ui::section("Layout", ftk::ui::icon::layout);
    if (auto t = ftk::ui::PropertyTable("##layout_rect")) {
        char text[64];
        const auto& r = node->layout_rect;
        std::snprintf(text, sizeof(text), "%.0f, %.0f px", r.x, r.y);
        ftk::ui::row("Position", [&] { ftk::ui::readout(text); });
        std::snprintf(text, sizeof(text), "%.0f \xc3\x97 %.0f px", r.w, r.h);
        ftk::ui::row("Size", [&] { ftk::ui::readout(text); });
        const auto& m = node->layout_margin;
        std::snprintf(text, sizeof(text), "%.0f %.0f %.0f %.0f", m.top, m.right, m.bottom, m.left);
        ftk::ui::row("Margin", "Top, right, bottom, left, in px as laid out.", [&] { ftk::ui::readout(text); });
        const auto& p = node->layout_padding;
        std::snprintf(text, sizeof(text), "%.0f %.0f %.0f %.0f", p.top, p.right, p.bottom, p.left);
        ftk::ui::row("Padding", "Top, right, bottom, left, in px as laid out.", [&] { ftk::ui::readout(text); });
    }

    // Style — editable. Edits write to the node's #id-selector rule in the
    // stylesheet (see SetStylePropertyCommand); values shown are the
    // resolved cascade output until something is set here, at which point
    // the value set here takes over.
    ftk::ui::section("Style", ftk::ui::icon::colour);
    if (node->type != UiNodeType::text) {
        draw_style_editor(*node);
    } else {
        ftk::ui::hint("Text takes its style from its parent element");
    }

    // Material — only nodes with a material="..." attribute have one.
    if (node->attributes.contains("material")) {
        draw_node_material_editor(*node);
    }
}

void UiEditorContext::draw_node_material_editor(UiNode& node) {
    if (!deps_.resource_manager || !deps_.vulkan_context) return;

    auto mat_it = node.attributes.find("material");
    if (mat_it == node.attributes.end() || mat_it->second.empty()) return;

    // Resolve {key} templates the same way GameUi::resolve_textures does:
    // walk up to the nearest scoped context, since a material name on a
    // widget instance is commonly a prop reference rather than a literal.
    std::string mat_name = mat_it->second;
    if (mat_name.find('{') != std::string::npos) {
        UiContext* ctx = nullptr;
        for (auto* n = &node; n; n = n->parent) {
            if (n->scoped_context) { ctx = n->scoped_context.get(); break; }
        }
        if (ctx) mat_name = UiDataBind::resolve_template(mat_name, *ctx);
    }
    if (mat_name.empty()) return;

    auto mat_handle = deps_.resource_manager->find_material(mat_name);
    if (!mat_handle.valid()) {
        std::string mat_path = mat_name;
        if (!mat_path.ends_with(".fjmat")) {
            mat_path = "assets/materials/" + mat_path + ".fjmat";
        }
        mat_path = deps_.project_mgr && deps_.project_mgr->active()
            ? deps_.project_mgr->active()->resolve(mat_path)
            : base_path::resolve(mat_path);
        if (fs::exists(mat_path)) {
            auto result = deps_.resource_manager->load_material_asset(
                mat_path, deps_.project_mgr ? deps_.project_mgr->active() : nullptr);
            if (result) mat_handle = *result;
        }
    }
    if (!mat_handle.valid()) return;

    auto* mat = deps_.resource_manager->get(mat_handle);
    if (!mat) return;

    auto* pipeline_cache = deps_.vulkan_context->pipeline_cache();
    if (!pipeline_cache) return;
    uint32_t pid = mat->pipeline_id();
    if (pid >= pipeline_cache->shader_count()) return;

    ftk::ui::section("Material", ftk::ui::icon::material);
    if (auto t = ftk::ui::PropertyTable("##material")) {
        ftk::ui::row("Shader", [&] { ftk::ui::readout(mat->shader_path().c_str()); });
    }

    // Param values live in the bindless SSBO and the fragment shader
    // samples them fresh every draw — but GameUi::update() stops issuing
    // new draw commands after a few idle frames (MAX_FRAMES_IN_FLIGHT
    // redraw countdown, see game_ui.cpp) once nothing marks the UI
    // dirty. A param edit doesn't touch the .fjui/.fjss tree so nothing
    // else would ever trip that flag — without mark_dirty() here, the
    // change lands in the SSBO but the preview simply stops re-drawing
    // before it ever samples it.
    const auto& meta = pipeline_cache->metadata(pid);
    if (draw_material_params(meta, deps_.vulkan_context->bindless_textures(), *mat)) {
        preview_ui_->mark_dirty();
    }
}

std::string UiEditorContext::style_prop_string(const UiNode& node, const char* property,
                                                const std::string& fallback) const {
    if (!preview_ui_) return fallback;
    for (const auto& rule : preview_ui_->stylesheet().rules) {
        if (!selects_only_id(rule.selector, node.id)) continue;
        if (const auto* decl = rule.find(property)) return decl->value;
    }
    return fallback;
}

void UiEditorContext::set_style(UiNode& node, const char* property, const std::string& before,
                                const std::string& after) {
    if (before == after) return;
    command_history().execute(std::make_unique<SetStylePropertyCommand>(
        *preview_ui_, node, property, before, after,
        std::string(after.empty() ? "Unset " : "Set ") + property));
}

void UiEditorContext::style_row_menu(UiNode& node, const char* property, const std::string& explicit_str) {
    const std::string popup = std::string("##unset_") + property;
    if (!explicit_str.empty() && ImGui::IsWindowHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        ImGui::TableGetHoveredRow() == ImGui::TableGetRowIndex()) {
        ImGui::OpenPopup(popup.c_str());
    }
    if (ImGui::BeginPopup(popup.c_str())) {
        if (ftk::ui::menu_item({.icon = ftk::ui::icon::clear, .label = "Unset"})) {
            set_style(node, property, explicit_str, {});
        }
        ImGui::EndPopup();
    }
}

void UiEditorContext::style_edit_float(UiNode& node, const char* property, float resolved_value,
                                       const ftk::ui::DragSpec& spec) {
    // Empty explicit_str means "no rule sets this yet" — kept empty (not
    // defaulted to the resolved value's string form) so an undo restores
    // that exact unset state via SetStylePropertyCommand's empty-value
    // erase, rather than pinning the resolved value as a new explicit rule.
    const auto explicit_str = style_prop_string(node, property, {});
    float source_value = resolved_value;
    if (!explicit_str.empty()) {
        try { source_value = std::stof(explicit_str); } catch (...) {}
    }
    ftk::ui::mark_next_row(!explicit_str.empty());
    ftk::ui::row(property, [&] {
        // While a drag is in progress the field shows (and accumulates into)
        // the buffered value, not source_value, which only reflects the last
        // committed edit: the resolved style doesn't move until the edit
        // commits, and re-deriving it mid-drag would snap the field back.
        const ImGuiID id = ImGui::GetID(property);
        auto it = active_float_edits_.find(id);
        float edited = it != active_float_edits_.end() ? it->second : source_value;
        const std::string field = std::string("##") + property;
        const auto edit = ftk::ui::drag(field.c_str(), edited, spec);
        style_origin_tooltip(property);
        if (edit.changed) active_float_edits_[id] = edited;
        if (edit.committed) {
            active_float_edits_.erase(id);
            if (edited != source_value) {
                set_style(node, property, explicit_str, to_css(UiLength::pixels(edited)));
            }
        }
        style_row_menu(node, property, explicit_str);
    });
}

void UiEditorContext::style_edit_length(UiNode& node, const char* property,
                                        UiLength resolved_value, float speed, float max) {
    const auto* desc = find_ui_style_property(property);
    if (!desc) return;

    // Same empty-means-unset convention as style_edit_float: the explicit
    // string is what undo restores to.
    const auto explicit_str = style_prop_string(node, property, {});
    const UiLength source = explicit_str.empty() ? resolved_value : StyleResolver::parse_length(explicit_str);

    // The units a length can take here, in the order the choice lists them.
    static constexpr UiUnit UNITS[] = {UiUnit::px, UiUnit::percent, UiUnit::em, UiUnit::rem,
                                       UiUnit::vw, UiUnit::vh, UiUnit::auto_};
    static constexpr const char* NAMES[] = {"px", "%", "em", "rem", "vw", "vh", "auto"};
    const std::span<const char* const> names(NAMES, desc->auto_allowed ? 7 : 6);

    ftk::ui::mark_next_row(!explicit_str.empty());
    ftk::ui::row(property, [&] {
        constexpr float UNIT_W = 72.0f;
        const float x = ImGui::GetCursorPosX();
        const float width = ImGui::GetContentRegionAvail().x;
        const float field_w = std::max(width - UNIT_W - ftk::theme::GAP_S, 1.0f);

        if (source.is_auto()) {
            // No number to drag: auto is the value, and the unit choice
            // beside it goes back to a number.
            ImGui::SetNextItemWidth(field_w);
            ftk::ui::readout("auto");
        } else {
            const ImGuiID id = ImGui::GetID(property);
            auto it = active_float_edits_.find(id);
            float edited = it != active_float_edits_.end() ? it->second : source.value;
            float limit = source.is_percent() ? 100.0f : source.is_relative() ? max / 16.0f : max;
            // Insets and margins may be negative; percentages of them too.
            const float min = desc->auto_allowed ? -limit : 0.0f;
            const float step = source.is_relative() ? speed / 16.0f : speed;
            ImGui::SetNextItemWidth(field_w);
            const std::string field = std::string("##") + property;
            const auto edit = ftk::ui::drag(field.c_str(), edited,
                                       {.speed = step, .min = min, .max = limit,
                                        .format = source.unit == UiUnit::px || source.is_percent() ? "%.1f" : "%.2f"});
            style_origin_tooltip(property);
            if (edit.changed) active_float_edits_[id] = edited;
            if (edit.committed) {
                active_float_edits_.erase(id);
                if (edited != source.value) set_style(node, property, explicit_str, to_css({edited, source.unit}));
            }
        }

        int unit = 0;
        for (int i = 0; i < static_cast<int>(names.size()); ++i) {
            if (UNITS[i] == source.unit) unit = i;
        }
        ImGui::SameLine();
        ImGui::SetCursorPosX(x + field_w + ftk::theme::GAP_S);
        ImGui::SetNextItemWidth(UNIT_W);
        const std::string choice = std::string("##unit_") + property;
        if (ftk::ui::choice(choice.c_str(), unit, names).changed) {
            const float value = source.is_auto() ? 0.0f : source.value;
            set_style(node, property, explicit_str,
                      UNITS[unit] == UiUnit::auto_ ? to_css(UiLength::automatic()) : to_css({value, UNITS[unit]}));
        }
        style_row_menu(node, property, explicit_str);
    });
}

void UiEditorContext::style_edit_color(UiNode& node, const char* property, const UiColor& resolved_value) {
    const auto explicit_str = style_prop_string(node, property, {});
    const UiColor source = explicit_str.empty() ? resolved_value : StyleResolver::parse_color(explicit_str);

    ftk::ui::mark_next_row(!explicit_str.empty());
    ftk::ui::row(property, [&] {
        // Same buffered-while-active pattern as style_edit_float — see there.
        const ImGuiID id = ImGui::GetID(property);
        auto it = active_color_edits_.find(id);
        const std::array<float, 4> rgba =
            it != active_color_edits_.end() ? it->second : std::array{source.r, source.g, source.b, source.a};
        glm::vec4 edited{rgba[0], rgba[1], rgba[2], rgba[3]};
        const std::string field = std::string("##") + property;
        const auto edit = ftk::ui::color(field.c_str(), edited, ftk::ui::ColorSpace::Srgb);
        style_origin_tooltip(property);
        if (edit.changed) active_color_edits_[id] = {edited.r, edited.g, edited.b, edited.a};
        if (edit.committed) {
            active_color_edits_.erase(id);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", static_cast<int>(edited.r * 255.0f + 0.5f),
                          static_cast<int>(edited.g * 255.0f + 0.5f), static_cast<int>(edited.b * 255.0f + 0.5f),
                          static_cast<int>(edited.a * 255.0f + 0.5f));
            set_style(node, property, explicit_str, buf);
        }
        style_row_menu(node, property, explicit_str);
    });
}

void UiEditorContext::style_edit_text(UiNode& node, const char* property, const std::string& resolved_value) {
    // Same empty-means-unset convention as style_edit_float: the explicit
    // string is what undo restores to.
    const auto explicit_str = style_prop_string(node, property, {});
    const std::string shown = explicit_str.empty() ? resolved_value : ui_style_unquote(explicit_str);

    ftk::ui::mark_next_row(!explicit_str.empty());
    ftk::ui::row(property, [&] {
        std::string value = shown;
        const std::string field = std::string("##") + property;
        const bool committed = ftk::ui::text_field(field.c_str(), value).committed;
        style_origin_tooltip(property);
        if (committed && value != shown) {
            // A name with a space needs quoting to read back as one name.
            if (value.find(' ') != std::string::npos) value = "\"" + value + "\"";
            set_style(node, property, explicit_str, value);
        }
        style_row_menu(node, property, explicit_str);
    });
}

void UiEditorContext::style_edit_keyword(UiNode& node, const char* property) {
    const auto* desc = find_ui_style_property(property);
    if (!desc || desc->kind != UiPropertyKind::keyword) return;

    // The keyword table's entries are string literals, so data() is a
    // null-terminated string the choice can list directly.
    std::vector<const char*> items;
    for (auto keyword : desc->keywords) items.push_back(keyword.data());

    int index = desc->get_keyword(node.style);
    const auto explicit_str = style_prop_string(node, property, {});
    if (!explicit_str.empty()) {
        for (size_t i = 0; i < items.size(); ++i) {
            if (explicit_str == items[i]) { index = static_cast<int>(i); break; }
        }
    }

    ftk::ui::mark_next_row(!explicit_str.empty());
    ftk::ui::row(property, [&] {
        int edited = index;
        const std::string field = std::string("##") + property;
        if (ftk::ui::choice(field.c_str(), edited, items).changed && edited != index) {
            set_style(node, property, explicit_str, items[static_cast<size_t>(edited)]);
        }
        style_origin_tooltip(property);
        style_row_menu(node, property, explicit_str);
    });
}

void UiEditorContext::style_origin_tooltip(const char* property) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) return;
    auto it = style_origins_.find(property);
    if (it == style_origins_.end()) {
        const auto* desc = find_ui_style_property(property);
        ImGui::SetTooltip("%s", desc && desc->inherited ? "Inherited from the parent: no rule sets it here"
                                                        : "The default: no rule sets it");
        return;
    }
    const auto& origin = it->second;
    if (origin.inline_style) {
        ImGui::SetTooltip("The style-%s attribute on this element", property);
        return;
    }
    auto file = fs::path(origin.rule->source).filename().string();
    if (file.empty()) file = "an unsaved rule";
    std::string media = origin.rule->media.empty() ? "" : "\n@media " + to_css(origin.rule->media);
    ImGui::SetTooltip("Set by %s in %s%s\n%s { %s: %s; }", to_css(origin.rule->selector).c_str(), file.c_str(),
                      media.c_str(), to_css(origin.rule->selector).c_str(), property,
                      origin.declaration->value.c_str());
}

void UiEditorContext::draw_style_editor(UiNode& node) {
    auto& s = node.style;
    style_origins_ = preview_ui_
        ? StyleResolver::explain(node, preview_ui_->stylesheet(),
                                 preview_ui_->layout_size().x, preview_ui_->layout_size().y)
        : std::unordered_map<std::string, StyleOrigin>{};
    // A drag or pick that ended without committing (Esc) leaves nothing
    // buffered once nothing is being edited.
    if (!ImGui::IsAnyItemActive()) {
        active_float_edits_.clear();
        active_color_edits_.clear();
    }

    if (ftk::ui::subheading_foldable("Layout")) {
        if (auto t = ftk::ui::PropertyTable("##style_layout")) {
            style_edit_keyword(node, "display");
            style_edit_keyword(node, "flex-direction");
            style_edit_keyword(node, "justify-content");
            style_edit_keyword(node, "align-items");
            style_edit_keyword(node, "align-self");
            style_edit_keyword(node, "flex-wrap");
            style_edit_keyword(node, "position");
            style_edit_keyword(node, "overflow");
            style_edit_float(node, "flex-grow", s.flex_grow, {.speed = 0.1f, .min = 0.0f, .max = 100.0f, .format = "%.1f"});
            style_edit_float(node, "flex-shrink", s.flex_shrink, {.speed = 0.1f, .min = 0.0f, .max = 100.0f, .format = "%.1f"});
        }
    }
    if (ftk::ui::subheading_foldable("Dimensions")) {
        if (auto t = ftk::ui::PropertyTable("##style_dimensions")) {
            style_edit_length(node, "width", s.width, 1.0f, 4096.0f);
            style_edit_length(node, "height", s.height, 1.0f, 4096.0f);
            style_edit_length(node, "min-width", s.min_width, 1.0f, 4096.0f);
            style_edit_length(node, "min-height", s.min_height, 1.0f, 4096.0f);
            style_edit_length(node, "max-width", s.max_width, 1.0f, 4096.0f);
            style_edit_length(node, "max-height", s.max_height, 1.0f, 4096.0f);
        }
    }
    if (ftk::ui::subheading_foldable("Spacing")) {
        if (auto t = ftk::ui::PropertyTable("##style_spacing")) {
            style_edit_length(node, "margin-top", s.margin.top, 1.0f, 512.0f);
            style_edit_length(node, "margin-right", s.margin.right, 1.0f, 512.0f);
            style_edit_length(node, "margin-bottom", s.margin.bottom, 1.0f, 512.0f);
            style_edit_length(node, "margin-left", s.margin.left, 1.0f, 512.0f);
            style_edit_length(node, "padding-top", s.padding.top, 1.0f, 512.0f);
            style_edit_length(node, "padding-right", s.padding.right, 1.0f, 512.0f);
            style_edit_length(node, "padding-bottom", s.padding.bottom, 1.0f, 512.0f);
            style_edit_length(node, "padding-left", s.padding.left, 1.0f, 512.0f);
            style_edit_length(node, "row-gap", s.row_gap, 1.0f, 256.0f);
            style_edit_length(node, "column-gap", s.column_gap, 1.0f, 256.0f);
        }
    }
    if (ftk::ui::subheading_foldable("Visual")) {
        if (auto t = ftk::ui::PropertyTable("##style_visual")) {
            style_edit_color(node, "background-color", s.background_color);
            style_edit_color(node, "color", s.color);
            style_edit_float(node, "font-size", s.font_size,
                             {.speed = 0.5f, .min = 1.0f, .max = 256.0f, .unit = ftk::ui::Unit::Pixels, .format = "%.1f"});
            style_edit_text(node, "font-family", s.font_family);
            style_edit_float(node, "font-weight", s.font_weight, {.speed = 10.0f, .min = 1.0f, .max = 1000.0f, .format = "%.0f"});
            style_edit_color(node, "border-color", s.border_color);
            style_edit_float(node, "border-width", s.border_width,
                             {.speed = 0.5f, .min = 0.0f, .max = 64.0f, .unit = ftk::ui::Unit::Pixels, .format = "%.1f"});
            style_edit_float(node, "border-radius", s.border_radius.top_left,
                             {.speed = 0.5f, .min = 0.0f, .max = 256.0f, .unit = ftk::ui::Unit::Pixels, .format = "%.1f"});
            style_edit_float(node, "opacity", s.opacity, {.speed = 0.01f, .min = 0.0f, .max = 1.0f});
        }
    }
}

// ── Mock Data Panel ──────────────────────────────────────────────────────

void UiEditorContext::draw_data() {
    auto panel = ftk::ui::Panel(ftk::ui::icon::ui_models, ctx_title("Data").c_str());
    if (!panel) return;
    if (!preview_ui_ || !preview_ui_->loaded() || !preview_data_) {
        ftk::ui::empty_state(ftk::ui::icon::ui_layout, "Nothing to feed", "The markup couldn't be loaded; the console says why");
        return;
    }
    const InspectorContext ctx{.resource_manager = deps_.resource_manager,
                               .project_mgr = deps_.project_mgr,
                               .icons = deps_.icon_cache};
    preview_data_->draw(*preview_ui_, ctx);
}

// ── Action Log Panel ─────────────────────────────────────────────────────

void UiEditorContext::draw_action_log() {
    auto panel = ftk::ui::Panel(ftk::ui::icon::console, ctx_title("Action log").c_str());
    if (!panel) return;
    if (action_log_.empty()) {
        ftk::ui::empty_state(ftk::ui::icon::console, "No actions yet", "Actions fired in Play mode are listed here");
        return;
    }
    ImGui::BeginDisabled(action_log_.empty());
    if (ftk::ui::action(ftk::ui::icon::clear, "Clear", ftk::ui::ButtonKind::Ghost)) action_log_.clear();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    char count[32];
    std::snprintf(count, sizeof(count), action_log_.size() == 1 ? "%zu action" : "%zu actions", action_log_.size());
    ftk::ui::readout(count);
    if (auto t = ftk::ui::PropertyTable("##log")) {
        for (auto it = action_log_.rbegin(); it != action_log_.rend(); ++it) {
            char at[32];
            std::snprintf(at, sizeof(at), "%.1f s", it->timestamp);
            ImGui::PushID(static_cast<int>(std::distance(action_log_.rbegin(), it)));
            ftk::ui::row(at, [&] { ftk::ui::readout(it->action.c_str()); });
            ImGui::PopID();
        }
    }
}

// ── Play Mode Input ──────────────────────────────────────────────────────

void UiEditorContext::process_play_input(UiNode* root, ImVec2 origin, float scale) {
    if (!ImGui::IsItemHovered()) return;

    ImVec2 mouse = ImGui::GetMousePos();
    float mx = (mouse.x - origin.x) / scale;
    float my = (mouse.y - origin.y) / scale;

    // Hit test: find deepest element node under cursor
    UiNode* hit = nullptr;
    std::function<void(UiNode*)> do_hit = [&](UiNode* node) {
        if (node->hidden_by_if) return;
        if (node->style.display == UiDisplay::none) return;
        auto& r = node->layout_rect;
        if (mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h) {
            if (node->type == UiNodeType::element) {
                hit = node;
            }
            for (auto& child : node->children) {
                do_hit(child.get());
            }
        }
    };
    do_hit(root);

    // Update hover state on all nodes
    std::function<void(UiNode*)> clear_hover = [&](UiNode* node) {
        bool was = node->is_hovered();
        node->interaction_state &= ~1u;
        if (was) {
            preview_ui_->mark_dirty();
        }
        for (auto& child : node->children) {
            clear_hover(child.get());
        }
    };
    clear_hover(root);

    // Set hover on hit node AND all ancestors (CSS hover bubbles up).
    // Only mark dirty on an actual state change — this runs every frame
    // the mouse sits over any element, and calling mark_dirty()
    // unconditionally here pins the UI permanently "dirty," forcing a
    // full style resolve every frame and starving out any signal from
    // real edits happening elsewhere (they're indistinguishable from
    // this constant background noise).
    for (UiNode* n = hit; n != nullptr; n = n->parent) {
        bool was = n->is_hovered();
        n->interaction_state |= 1; // hovered
        if (!was) {
            preview_ui_->mark_dirty();
        }
    }

    // Active state (mouse down)
    bool mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool mouse_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    // Clear active on all nodes when mouse is up
    if (!mouse_down) {
        std::function<void(UiNode*)> clear_active = [&](UiNode* node) {
            if (node->is_active()) {
                node->interaction_state &= ~2u;
                preview_ui_->mark_dirty();
            }
            for (auto& child : node->children) {
                clear_active(child.get());
            }
        };
        clear_active(root);
    } else if (hit) {
        // Set active on hit + ancestors — only mark dirty on nodes that
        // weren't already active, same reasoning as the hover loop above.
        for (UiNode* n = hit; n != nullptr; n = n->parent) {
            bool was = n->is_active();
            n->interaction_state |= 2; // active
            if (!was) {
                preview_ui_->mark_dirty();
            }
        }
    }

    // Dispatch on:click — walk up from hit to find nearest handler
    if (mouse_clicked && hit) {
        for (UiNode* n = hit; n != nullptr; n = n->parent) {
            auto it = n->attributes.find("on:click");
            if (it != n->attributes.end()) {
                preview_ui_->context().dispatch(it->second);
                break;
            }
        }
    }
}

// ── Layout Debug Overlay ─────────────────────────────────────────────────

namespace {

bool drawn(const UiNode& node) {
    return !node.hidden_by_if && node.style.display != UiDisplay::none;
}

} // namespace

UiNode* UiEditorContext::node_at(UiNode* root, ImVec2 mouse, ImVec2 origin, float scale) {
    const float x = (mouse.x - origin.x) / scale;
    const float y = (mouse.y - origin.y) / scale;
    UiNode* hit = nullptr;
    std::function<void(UiNode*)> visit = [&](UiNode* node) {
        if (!drawn(*node)) return;
        const auto& r = node->layout_rect;
        if (x < r.x || x > r.x + r.w || y < r.y || y > r.y + r.h) return;
        hit = node;
        for (auto& child : node->children) visit(child.get());
    };
    visit(root);
    return hit;
}

void UiEditorContext::draw_layout_overlay(UiNode* root, UiNode* hovered, ImDrawList* draw_list, ImVec2 origin,
                                          float scale) {
    const ImU32 box = ImGui::GetColorU32(ftk::theme::layout_box());
    const auto screen = [&](const UiRect& r) {
        return std::pair{ImVec2{origin.x + r.x * scale, origin.y + r.y * scale},
                         ImVec2{origin.x + (r.x + r.w) * scale, origin.y + (r.y + r.h) * scale}};
    };

    // Every element's box, outlined.
    std::function<void(const UiNode*)> outline = [&](const UiNode* node) {
        if (!drawn(*node)) return;
        const auto& r = node->layout_rect;
        if (r.w >= 0.5f || r.h >= 0.5f) {
            const auto [p0, p1] = screen(r);
            draw_list->AddRect(p0, p1, box);
        }
        for (const auto& child : node->children) outline(child.get());
    };
    outline(root);

    // The element pointed at and the selected one: margin and padding
    // shaded, their sizes written in them, the tag and size beside the box.
    const auto detail = [&](const UiNode* node) {
        if (node == nullptr || !drawn(*node)) return;
        const auto [p0, p1] = screen(node->layout_rect);
        const auto& m = node->layout_margin;
        const auto& pd = node->layout_padding;
        const ImVec2 m0{p0.x - m.left * scale, p0.y - m.top * scale};
        const ImVec2 m1{p1.x + m.right * scale, p1.y + m.bottom * scale};
        const ImVec2 i0{p0.x + pd.left * scale, p0.y + pd.top * scale};
        const ImVec2 i1{p1.x - pd.right * scale, p1.y - pd.bottom * scale};
        // A ring between an outer and an inner rectangle, as four strips.
        const auto ring = [&](ImVec2 o0, ImVec2 o1, ImVec2 n0, ImVec2 n1, ImU32 fill) {
            draw_list->AddRectFilled(o0, {o1.x, n0.y}, fill);
            draw_list->AddRectFilled({o0.x, n1.y}, o1, fill);
            draw_list->AddRectFilled({o0.x, n0.y}, {n0.x, n1.y}, fill);
            draw_list->AddRectFilled({n1.x, n0.y}, {o1.x, n1.y}, fill);
        };
        ring(m0, m1, p0, p1, ImGui::GetColorU32(ftk::theme::layout_margin()));
        ring(p0, p1, i0, i1, ImGui::GetColorU32(ftk::theme::layout_padding()));
        draw_list->AddRect(p0, p1, box, 0.0f, 0, 1.5f);

        // A side's size, where the strip is tall enough to hold it.
        ImGui::PushFont(ftk::theme::mono_font(), ftk::theme::AXIS_TEXT);
        const float text_h = ImGui::GetFontSize();
        const auto size_in = [&](float value, ImVec2 a, ImVec2 b) {
            if (value <= 0.0f || std::min(b.x - a.x, b.y - a.y) < text_h) return;
            char text[16];
            std::snprintf(text, sizeof(text), "%g", value);
            const ImVec2 ts = ImGui::CalcTextSize(text);
            draw_list->AddText({(a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f},
                               ImGui::GetColorU32(ftk::theme::text()), text);
        };
        size_in(m.top, m0, {m1.x, p0.y});
        size_in(m.bottom, {m0.x, p1.y}, m1);
        size_in(m.left, {m0.x, p0.y}, {p0.x, p1.y});
        size_in(m.right, {p1.x, p0.y}, {m1.x, p1.y});
        size_in(pd.top, p0, {p1.x, i0.y});
        size_in(pd.bottom, {p0.x, i1.y}, p1);
        size_in(pd.left, {p0.x, i0.y}, {i0.x, i1.y});
        size_in(pd.right, {i1.x, i0.y}, {p1.x, i1.y});
        ImGui::PopFont();

        char label[96];
        std::snprintf(label, sizeof(label), "%s  %g \xc3\x97 %g", node->tag.c_str(), node->layout_rect.w,
                      node->layout_rect.h);
        ftk::ui::canvas_readout(draw_list, {p1.x, p0.y}, label, draw_list->GetClipRectMin(), draw_list->GetClipRectMax());
    };
    if (hovered != selected_node_) detail(hovered);
    detail(selected_node_);
}

} // namespace fjell
