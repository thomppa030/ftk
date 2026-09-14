#include "ui/ui_editor_context.hpp"
#include "ui/dockspace_layout.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/ui_data_bind.hpp"
#include "ui/game_ui/fjui_serializer.hpp"
#include "ui/game_ui/ui_style_commands.hpp"
#include "ui/game_ui/ui_style_properties.hpp"
#include "ui/game_ui/widget_asset.hpp"
#include "ui/game_ui/ui_context.hpp"
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
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/icon_cache.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fjell {

namespace fs = std::filesystem;

UiEditorContext::UiEditorContext(UiEditorDeps deps)
    : deps_{deps} {}

void UiEditorContext::init() {
    // Preview GameUi is initialized lazily when a .fjui file is opened

    init_shared_panels(deps_.vulkan_context, deps_.project_mgr);
    history_panel().init(&command_history());
}

void UiEditorContext::shutdown() {
    // Clear preview callback before destroying resources
    deps_.vulkan_context->set_ui_preview_callback(nullptr);

    // Clear the UiContext's action delegates before disconnecting connections.
    if (preview_ui_) {
        preview_ui_->context().clear();
    }
    action_connections_.clear();
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

bool UiEditorContext::save() {
    if (!preview_ui_ || !preview_ui_->root() || fjui_path_.empty()) return false;
    auto markup_path = base_path::resolve_asset(fjui_path_, deps_.project_mgr->active());

    // The watcher would otherwise see these writes and reopen the files we
    // are saving, throwing away selection and undo history for nothing.
    preview_ui_->expect_write(markup_path);
    preview_ui_->expect_write(fjss_path_);

    if (is_widget_file_) {
        auto result = save_widget_asset(markup_path, loaded_widget_, preview_ui_->root());
        if (!result) {
            FJELL_CORE_WARN("UI editor: {}", result.error());
            return false;
        }
    } else {
        std::ofstream out(markup_path);
        if (!out) {
            FJELL_CORE_WARN("UI editor: cannot write {}", markup_path);
            return false;
        }
        out << FjuiSerializer::serialize(*preview_ui_->root());
    }

    if (!fjss_path_.empty()) {
        std::ofstream out(fjss_path_);
        if (!out) {
            FJELL_CORE_WARN("UI editor: cannot write {}", fjss_path_);
            return false;
        }
        out << FjssSerializer::serialize(preview_ui_->stylesheet(), fjss_path_);
    }
    FJELL_CORE_INFO("UI editor: saved {} and {}", markup_path, fjss_path_);
    return true;
}

void UiEditorContext::draw(float dt) {
    elapsed_time_ += dt;

    // A source file changed on disk: rebuild the preview from the files.
    // Inspector edits live only in the in-memory stylesheet and do not
    // survive this, and the history's commands point into the old tree.
    if (preview_ui_ && preview_ui_->consume_reload_request()) {
        command_history().clear();
        open_ui(fjui_path_);
    }

    draw_preview(dt);
    draw_element_tree();
    draw_inspector();
    draw_mock_data();
    draw_action_log();

    draw_shared_panels(dt);
}

std::vector<std::string> UiEditorContext::docked_window_names() const {
    return {ctx_title("Element Tree"), ctx_title("UI Preview"),
            ctx_title("UI Inspector"), ctx_title("Mock Data"), ctx_title("Action Log"),
            ctx_title("History"), ctx_title("Stats"), ctx_title("Console")};
}

void UiEditorContext::setup_dockspace(ImGuiID main_area) {
    enum class EPanel { ElementTree, UiPreview, Inspector, MockData, ActionLog, History, Stats, Console };
    using N = DockNode;
    static const auto layout = N::horizontal_split(0.16f,
        N::leaf({EPanel::ElementTree}),
        N::horizontal_split(0.76f,
            N::vertical_split(0.75f,
                N::leaf({EPanel::UiPreview}),
                N::leaf({EPanel::ActionLog, EPanel::History, EPanel::Stats, EPanel::Console})),
            N::vertical_split(0.55f,
                N::leaf({EPanel::Inspector}),
                N::leaf({EPanel::MockData}))));
    build_dockspace(main_area, layout, docked_window_names());
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
            FJELL_CORE_WARN("UiEditor: {}", result.error());
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
        if (deps_.project_mgr && deps_.project_mgr->active()) {
            const auto& ui = deps_.project_mgr->active()->config().ui;
            preview_width_ = static_cast<float>(ui.design_width);
            preview_height_ = static_cast<float>(ui.design_height);
        }
        if (deps_.engine_game_ui) {
            // Share font atlas; use offscreen color format (not swapchain format)
            deps_.vulkan_context->init_game_ui_shared(*preview_ui_,
                                                       deps_.engine_game_ui->font(),
                                                       OffscreenPass::COLOR_FORMAT);
        } else {
            deps_.vulkan_context->init_game_ui(*preview_ui_,
                base_path::resolve("engine_assets/fonts/Inter-Regular.ttf"),
                OffscreenPass::COLOR_FORMAT);
        }
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
            offscreen_[i] = std::make_unique<OffscreenPass>(
                deps_.vulkan_context->device_ref(), deps_.vulkan_context->allocator(),
                800, 600, VK_SAMPLE_COUNT_1_BIT);
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

    // Load the UI — either from inline markup tree or from .fjui file
    if (is_widget_file && loaded_widget_.markup_root) {
        auto tree_clone = UiDataBind::clone_node(*loaded_widget_.markup_root, nullptr);
        auto base_dir = fs::path(abs_path).parent_path().string();
        preview_ui_->load_from_tree(std::move(tree_clone), fjss_path_, base_dir, abs_path);
    } else {
        preview_ui_->load(abs_path, fjss_path_);
    }

    // Apply mock data
    for (const auto& [key, value] : mock_entries_) {
        preview_ui_->context().set(key, value);
    }

    // Bind action logging — scan tree for on:click attributes and log dispatches
    action_connections_.clear();
    action_log_.clear();
    auto* root = preview_ui_->root();
    if (root) {
        // Collect action names and auto-generate bindings from class:name="key==value"
        discovered_actions_.clear();
        auto& action_names = discovered_actions_;
        std::function<void(UiNode*)> collect_actions = [&](UiNode* node) {
            auto click_it = node->attributes.find("on:click");
            if (click_it != node->attributes.end() && !click_it->second.empty()) {
                const auto& action = click_it->second;
                action_names.push_back(action);

                // Auto-bind: if same node has class:X="key==value", infer action → set key=value
                for (const auto& [attr, condition] : node->attributes) {
                    if (attr.starts_with("class:")) {
                        auto eq = condition.find("==");
                        if (eq != std::string::npos) {
                            std::string key = condition.substr(0, eq);
                            std::string value = condition.substr(eq + 2);
                            // Only add if not already manually bound
                            bool exists = false;
                            for (const auto& b : action_bindings_) {
                                if (b.action == action && b.key == key) {
                                    exists = true;
                                    break;
                                }
                            }
                            if (!exists) {
                                action_bindings_.push_back({action, key, value});
                            }
                        }
                    }
                }
            }
            for (auto& child : node->children) {
                collect_actions(child.get());
            }
        };
        collect_actions(root);

        action_connections_.reserve(action_names.size());
        for (const auto& action_name : action_names) {
            action_connections_.push_back(
                preview_ui_->context().bind_action(action_name,
                    [this, action_name]() {
                        action_log_.push_back({action_name, elapsed_time_});
                        for (const auto& bind : action_bindings_) {
                            if (bind.action == action_name) {
                                preview_ui_->context().set(bind.key, bind.value);
                                preview_ui_->mark_dirty();
                            }
                        }
                    }));
        }
    }

    // Detect widget isolation mode
    is_widget_file_ = is_widget_file ||
                      fjui_path_.find("/widgets/") != std::string::npos ||
                      fjui_path_.find("\\widgets\\") != std::string::npos;

    // Populate mock entries from declared props (with defaults)
    if (!loaded_widget_.props.empty()) {
        for (const auto& prop : loaded_widget_.props) {
            // Only add if not already present in mock_entries_
            bool exists = false;
            for (const auto& [k, v] : mock_entries_) {
                if (k == prop.name) { exists = true; break; }
            }
            if (!exists) {
                std::string val = prop.default_value.empty() ? "" : prop.default_value;
                mock_entries_.emplace_back(prop.name, val);
                preview_ui_->context().set(prop.name, val);
            }
        }
        preview_ui_->mark_dirty();
    }

    // Auto-discover {key} prop templates in the widget
    discovered_props_.clear();
    if (is_widget_file_ && root) {
        std::function<void(const UiNode*)> collect_props = [&](const UiNode* n) {
            // Scan text templates
            if (!n->text_template.empty()) {
                size_t pos = 0;
                while (pos < n->text_template.size()) {
                    auto open = n->text_template.find('{', pos);
                    if (open == std::string::npos) break;
                    auto close = n->text_template.find('}', open + 1);
                    if (close == std::string::npos) break;
                    auto key = n->text_template.substr(open + 1, close - open - 1);
                    if (std::find(discovered_props_.begin(), discovered_props_.end(), key)
                        == discovered_props_.end()) {
                        discovered_props_.push_back(key);
                    }
                    pos = close + 1;
                }
            }
            // Scan value/max attributes on bar elements
            for (const auto& [attr, val] : n->attributes) {
                if (val.find('{') == std::string::npos) continue;
                size_t pos = 0;
                while (pos < val.size()) {
                    auto open = val.find('{', pos);
                    if (open == std::string::npos) break;
                    auto close = val.find('}', open + 1);
                    if (close == std::string::npos) break;
                    auto key = val.substr(open + 1, close - open - 1);
                    if (std::find(discovered_props_.begin(), discovered_props_.end(), key)
                        == discovered_props_.end()) {
                        discovered_props_.push_back(key);
                    }
                    pos = close + 1;
                }
            }
            for (const auto& child : n->children) {
                collect_props(child.get());
            }
        };
        collect_props(root);

        // Auto-create mock entries for discovered props that don't exist yet
        for (const auto& prop : discovered_props_) {
            bool exists = false;
            for (const auto& [k, v] : mock_entries_) {
                if (k == prop) { exists = true; break; }
            }
            if (!exists) {
                mock_entries_.emplace_back(prop, "");
            }
        }
    }

    selected_node_ = nullptr;
    FJELL_CORE_INFO("UI editor opened: {}", fjui_path_);
}

// ── Preview Panel ────────────────────────────────────────────────────────

void UiEditorContext::draw_preview(float dt) {
    if (ImGui::Begin(ctx_title("UI Preview").c_str())) {
        if (!preview_ui_ || !preview_ui_->loaded()) {
            ImGui::TextDisabled("No .fjui loaded");
            ImGui::End();
            return;
        }

        // Resolution controls
        ImGui::Text("Resolution:");
        ImGui::SameLine();
        if (deps_.project_mgr && deps_.project_mgr->active()) {
            const auto& ui = deps_.project_mgr->active()->config().ui;
            std::string design = std::to_string(ui.design_width) + "x" + std::to_string(ui.design_height);
            if (ImGui::SmallButton(design.c_str())) {
                preview_width_ = static_cast<float>(ui.design_width);
                preview_height_ = static_cast<float>(ui.design_height);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The project's design resolution");
            ImGui::SameLine();
            ImGui::Checkbox("Design scale", &design_scale_);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Scale the preview by the design height, as a screen-space\n"
                                  "placement does in game. Turn off for world-space widgets.");
            }
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("800x600")) { preview_width_ = 800; preview_height_ = 600; }
        ImGui::SameLine();
        if (ImGui::SmallButton("1280x720")) { preview_width_ = 1280; preview_height_ = 720; }
        ImGui::SameLine();
        if (ImGui::SmallButton("1920x1080")) { preview_width_ = 1920; preview_height_ = 1080; }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::DragFloat("##pw", &preview_width_, 1.0f, 100.0f, 3840.0f, "%.0f");
        ImGui::SameLine();
        ImGui::Text("x");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::DragFloat("##ph", &preview_height_, 1.0f, 100.0f, 2160.0f, "%.0f");

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Edit / Play mode toggle
        bool was_play = play_mode_;
        if (was_play) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.2f, 0.5f, 0.2f, 1.0f});
        }
        if (ImGui::SmallButton(play_mode_ ? "Play" : "Edit")) {
            play_mode_ = !play_mode_;
        }
        if (was_play) {
            ImGui::PopStyleColor();
        }

        if (!play_mode_) {
            ImGui::SameLine();
            ImGui::Checkbox("Layout", &show_layout_overlay_);
        }

        // Resolution changed — resize all offscreen passes
        uint32_t new_w = static_cast<uint32_t>(preview_width_);
        uint32_t new_h = static_cast<uint32_t>(preview_height_);
        if ((new_w != offscreen_width_ || new_h != offscreen_height_) &&
            new_w > 0 && new_h > 0) {
            for (auto& pass : offscreen_) {
                if (pass) pass->resize(new_w, new_h);
            }
            offscreen_width_ = new_w;
            offscreen_height_ = new_h;
            preview_ui_->mark_dirty();
        }

        ImGui::Separator();

        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.x < 1.0f || avail.y < 1.0f) {
            ImGui::End();
            return;
        }

        // Re-apply mock data each frame (ensures values persist after hot-reload)
        for (const auto& [key, value] : mock_entries_) {
            preview_ui_->context().set(key, value);
        }
        sync_mock_lists();

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
            VkDescriptorSet tex = VK_NULL_HANDLE;
            uint32_t latest = latest_rendered_offscreen_.load(std::memory_order_acquire);
            if (latest < PREVIEW_FRAMES && offscreen_[latest]) {
                tex = offscreen_[latest]->imgui_texture();
            }
            if (!tex) {
                // Fall back to any valid texture before the first real
                // render has completed (e.g. the very first frame).
                for (uint32_t i = 0; i < PREVIEW_FRAMES; ++i) {
                    if (offscreen_[i]) {
                        tex = offscreen_[i]->imgui_texture();
                        if (tex) break;
                    }
                }
            }
            if (tex) {
                ImGui::SetCursorScreenPos(origin);
                ImGui::Image(reinterpret_cast<ImTextureID>(tex),
                             {scaled_w, scaled_h});
            }
        }

        // Overlay: layout debug (Edit mode only, drawn via ImGui DrawList on top)
        auto* root = preview_ui_->root();
        if (!play_mode_ && show_layout_overlay_ && root) {
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            draw_layout_overlay(root, draw_list, origin, layout_scale);
        }

        // Invisible button for interaction (placed over the image)
        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##preview_area", {scaled_w, scaled_h});

        if (play_mode_) {
            // Play mode: forward mouse events to GameUi for interaction
            if (root) {
                process_play_input(root, origin, layout_scale);
            }
        } else {
            // Edit mode: click-to-select elements
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ImVec2 mouse = ImGui::GetMousePos();
                float local_x = (mouse.x - origin.x) / layout_scale;
                float local_y = (mouse.y - origin.y) / layout_scale;

                UiNode* hit = nullptr;
                std::function<void(UiNode*)> hit_test = [&](UiNode* node) {
                    if (node->hidden_by_if) return;
                    if (node->style.display == UiDisplay::none) return;

                    auto& r = node->layout_rect;
                    if (local_x >= r.x && local_x <= r.x + r.w &&
                        local_y >= r.y && local_y <= r.y + r.h) {
                        hit = node;
                        for (auto& child : node->children) {
                            hit_test(child.get());
                        }
                    }
                };
                if (root) hit_test(root);
                selected_node_ = hit;
            }
        }
    }
    ImGui::End();
}

void UiEditorContext::draw_node_preview(UiNode* node, ImDrawList* draw_list,
                                         ImVec2 origin, float scale) {
    if (node->hidden_by_if) return;
    if (node->style.display == UiDisplay::none) return;

    auto& r = node->layout_rect;
    if (r.w < 0.5f || r.h < 0.5f) {
        // Still draw children (root may have 0 size)
        for (auto& child : node->children) {
            draw_node_preview(child.get(), draw_list, origin, scale);
        }
        return;
    }

    ImVec2 p0 = {origin.x + r.x * scale, origin.y + r.y * scale};
    ImVec2 p1 = {p0.x + r.w * scale, p0.y + r.h * scale};

    auto& bg = node->style.background_color;
    bool has_bg = bg.a > 0.01f;

    // Draw background
    if (has_bg) {
        auto col = IM_COL32(
            static_cast<int>(bg.r * 255), static_cast<int>(bg.g * 255),
            static_cast<int>(bg.b * 255), static_cast<int>(bg.a * 255));
        float rounding = node->style.border_radius.top_left * scale;
        draw_list->AddRectFilled(p0, p1, col, rounding);
    }

    // Draw border
    auto& bc = node->style.border_color;
    float bw = node->style.border_width;
    if (bw > 0.0f && bc.a > 0.01f) {
        auto col = IM_COL32(
            static_cast<int>(bc.r * 255), static_cast<int>(bc.g * 255),
            static_cast<int>(bc.b * 255), static_cast<int>(bc.a * 255));
        float rounding = node->style.border_radius.top_left * scale;
        draw_list->AddRect(p0, p1, col, rounding, 0, bw * scale);
    }

    // Draw text content
    if (node->type == UiNodeType::text && !node->text_content.empty()) {
        auto& tc = node->style.color;
        auto text_col = IM_COL32(
            static_cast<int>(tc.r * 255), static_cast<int>(tc.g * 255),
            static_cast<int>(tc.b * 255), static_cast<int>(tc.a * 255));
        float font_size = node->style.font_size * scale;
        if (font_size >= 4.0f) {
            // Use parent's layout rect for text positioning
            UiNode* layout_node = node->parent ? node->parent : node;
            auto& lr = layout_node->layout_rect;
            ImVec2 text_pos = {
                origin.x + (lr.x + layout_node->layout_padding.left) * scale,
                origin.y + (lr.y + layout_node->layout_padding.top) * scale
            };
            draw_list->AddText(nullptr, font_size, text_pos, text_col,
                               node->text_content.c_str());
        }
    }

    // Highlight selected node
    if (node == selected_node_) {
        draw_list->AddRect(p0, p1, IM_COL32(100, 180, 255, 200), 0, 0, 2.0f);

        // Margin overlay (orange)
        auto& m = node->layout_margin;
        if (m.top > 0 || m.right > 0 || m.bottom > 0 || m.left > 0) {
            ImVec2 mp0 = {p0.x - m.left * scale, p0.y - m.top * scale};
            ImVec2 mp1 = {p1.x + m.right * scale, p1.y + m.bottom * scale};
            draw_list->AddRect(mp0, mp1, IM_COL32(255, 165, 0, 120), 0, 0, 1.0f);
        }

        // Padding overlay (green)
        auto& pd = node->layout_padding;
        if (pd.top > 0 || pd.right > 0 || pd.bottom > 0 || pd.left > 0) {
            ImVec2 pp0 = {p0.x + pd.left * scale, p0.y + pd.top * scale};
            ImVec2 pp1 = {p1.x - pd.right * scale, p1.y - pd.bottom * scale};
            draw_list->AddRect(pp0, pp1, IM_COL32(100, 200, 100, 120), 0, 0, 1.0f);
        }
    }

    // Children
    for (auto& child : node->children) {
        draw_node_preview(child.get(), draw_list, origin, scale);
    }
}

// ── Element Tree Panel ───────────────────────────────────────────────────

void UiEditorContext::draw_element_tree() {
    if (ImGui::Begin(ctx_title("Element Tree").c_str())) {
        if (!preview_ui_ || !preview_ui_->loaded()) {
            ImGui::TextDisabled("No .fjui loaded");
            ImGui::End();
            return;
        }

        auto* root = preview_ui_->root();
        if (root) {
            draw_node_tree(root);
        }
    }
    ImGui::End();
}

void UiEditorContext::draw_node_tree(UiNode* node) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;

    if (node->children.empty() && node->type != UiNodeType::text) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (node == selected_node_) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    // Skip foreach template nodes (hidden, only clones are visible)
    if (node->is_foreach_template) return;

    // Build display label: <tag> .class #id "text..."
    std::string label;
    if (node->type == UiNodeType::text) {
        // Text nodes: show truncated content
        auto text = node->text_content;
        if (text.size() > 30) {
            text = text.substr(0, 27) + "...";
        }
        label = "\"" + text + "\"";
        flags |= ImGuiTreeNodeFlags_Leaf;
    } else if (node->is_widget) {
        // Widget nodes: show original tag name with widget marker
        label = "<" + node->widget_tag + "/>";
        if (!node->id.empty()) {
            label += " #" + node->id;
        }
    } else if (node->foreach_item_index >= 0) {
        // Foreach clones: show index
        label = "<" + node->tag + "> [" + std::to_string(node->foreach_item_index) + "]";
    } else {
        label = "<" + (node->tag.empty() ? "root" : node->tag) + ">";
        if (!node->id.empty()) {
            label += " #" + node->id;
        }
        for (const auto& cls : node->classes) {
            // Skip auto-generated scope classes
            if (cls.starts_with("__w_")) continue;
            label += " ." + cls;
        }
    }

    // Color widget nodes purple, foreach items teal
    bool color_pushed = false;
    if (node->is_widget) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.6f, 0.45f, 0.8f, 1.0f});
        color_pushed = true;
    } else if (node->foreach_item_index >= 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.3f, 0.75f, 0.7f, 1.0f});
        color_pushed = true;
    } else if (node->hidden_by_if || node->style.display == UiDisplay::none) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        color_pushed = true;
    }

    bool open = ImGui::TreeNodeEx(node, flags, "%s", label.c_str());

    if (color_pushed) {
        ImGui::PopStyleColor();
    }

    if (ImGui::IsItemClicked()) {
        selected_node_ = node;
    }

    if (open) {
        for (auto& child : node->children) {
            draw_node_tree(child.get());
        }
        ImGui::TreePop();
    }
}

// ── Inspector Panel ──────────────────────────────────────────────────────

void UiEditorContext::draw_stylesheet_problems() {
    if (!preview_ui_) return;
    const auto& problems = preview_ui_->stylesheet().diagnostics;
    if (problems.empty()) return;

    ImGui::SeparatorText("Problems");
    for (const auto& d : problems) {
        ImVec4 color = d.severity == FjssSeverity::error
            ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
            : ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
        auto file = fs::path(d.source).filename().string();
        ImGui::TextColored(color, "%s:%u:%u", file.c_str(), d.line, d.column);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", d.message.c_str());
    }
    ImGui::Spacing();
}

void UiEditorContext::draw_inspector() {
    if (ImGui::Begin(ctx_title("UI Inspector").c_str())) {
        // Stylesheet problems show up next to the UI they broke, even with
        // nothing selected — a typo in a hot-reloaded sheet should be the
        // first thing visible, not something to dig out of the console.
        draw_stylesheet_problems();

        if (!selected_node_) {
            ImGui::TextDisabled("Select an element in the tree or preview");
            ImGui::End();
            return;
        }

        auto* node = selected_node_;

        // Element info
        ImGui::SeparatorText("Element");
        ImGui::Text("Tag: %s", node->tag.empty() ? "(root)" : node->tag.c_str());
        if (!node->id.empty()) {
            ImGui::Text("ID: %s", node->id.c_str());
        }
        if (!node->classes.empty()) {
            std::string cls_str;
            for (const auto& c : node->classes) {
                if (c.starts_with("__w_")) continue; // skip scope classes
                if (!cls_str.empty()) cls_str += ", ";
                cls_str += "." + c;
            }
            if (!cls_str.empty()) {
                ImGui::Text("Classes: %s", cls_str.c_str());
            }
        }
        if (node->type == UiNodeType::text) {
            ImGui::TextWrapped("Text: %s", node->text_content.c_str());
        }
        if (!node->text_template.empty()) {
            ImGui::TextDisabled("Template: %s", node->text_template.c_str());
        }
        if (node->hidden_by_if) {
            ImGui::TextColored({1.0f, 0.5f, 0.3f, 1.0f}, "Hidden by if directive");
        }

        // Widget info
        if (node->is_widget) {
            ImGui::SeparatorText("Widget");
            ImGui::TextColored({0.6f, 0.45f, 0.8f, 1.0f}, "<%s/>", node->widget_tag.c_str());

            if (!node->prop_map.empty()) {
                ImGui::Text("Props:");
                ImGui::Indent();
                for (const auto& [inner, outer] : node->prop_map) {
                    ImGui::Text("%s = %s", inner.c_str(), outer.c_str());
                }
                ImGui::Unindent();
            }

            if (!node->event_map.empty()) {
                ImGui::Text("Events:");
                ImGui::Indent();
                for (const auto& [event, action] : node->event_map) {
                    ImGui::Text("on:%s -> %s", event.c_str(), action.c_str());
                }
                ImGui::Unindent();
            }

            if (node->scoped_context) {
                ImGui::TextDisabled("(scoped context active)");
            }

            // Controller info
            if (deps_.script_manager && deps_.script_manager->is_loaded()) {
                auto* ui_type = deps_.script_manager->find_ui_type_by_tag(node->widget_tag);
                if (ui_type) {
                    ImGui::Spacing();
                    ImGui::TextColored({0.4f, 0.8f, 0.4f, 1.0f}, "Controller: %s",
                                       ui_type->name.c_str());
                    if (!ui_type->fields.empty()) {
                        ImGui::Text("Bind fields:");
                        ImGui::Indent();
                        for (const auto& f : ui_type->fields) {
                            const char* type_name = "?";
                            switch (f.type) {
                                case FJELL_FIELD_FLOAT: type_name = "float"; break;
                                case FJELL_FIELD_INT: type_name = "int"; break;
                                case FJELL_FIELD_BOOL: type_name = "bool"; break;
                                case FJELL_FIELD_STRING: type_name = "string"; break;
                                case FJELL_FIELD_VEC2: type_name = "vec2"; break;
                                case FJELL_FIELD_VEC3: type_name = "vec3"; break;
                                case FJELL_FIELD_VEC4: type_name = "vec4"; break;
                                default: break;
                            }
                            ImGui::Text("%s %s", type_name, f.name.c_str());
                        }
                        ImGui::Unindent();
                    }
                } else {
                    ImGui::TextDisabled("No controller registered");
                }
            }
        }

        // Foreach info
        if (node->foreach_item_index >= 0) {
            ImGui::SeparatorText("Foreach");
            ImGui::TextColored({0.3f, 0.75f, 0.7f, 1.0f}, "Item [%d]", node->foreach_item_index);
            if (node->foreach_context) {
                ImGui::TextDisabled("(item context active)");
            }
        }

        // Attributes
        if (!node->attributes.empty()) {
            ImGui::SeparatorText("Attributes");
            for (const auto& [key, value] : node->attributes) {
                if (key.starts_with("_")) continue; // skip internal attributes
                ImGui::Text("%s: %s", key.c_str(), value.c_str());
            }
        }

        // Interaction state
        {
            ImGui::SeparatorText("State");
            ImGui::Text("Hovered: %s", node->is_hovered() ? "yes" : "no");
            ImGui::Text("Active: %s", node->is_active() ? "yes" : "no");
            ImGui::Text("Focused: %s", node->is_focused() ? "yes" : "no");
        }

        // Layout rect
        {
            ImGui::SeparatorText("Layout");
            auto& r = node->layout_rect;
            ImGui::Text("Position: %.0f, %.0f", r.x, r.y);
            ImGui::Text("Size: %.0f x %.0f", r.w, r.h);

            auto& m = node->layout_margin;
            ImGui::Text("Margin: %.0f %.0f %.0f %.0f", m.top, m.right, m.bottom, m.left);
            auto& p = node->layout_padding;
            ImGui::Text("Padding: %.0f %.0f %.0f %.0f", p.top, p.right, p.bottom, p.left);
        }

        // Style — editable. Edits write to the node's #id-selector rule
        // in the stylesheet (see SetStylePropertyCommand); values shown
        // are the resolved cascade output until something is explicitly
        // set here, at which point the explicit value takes over.
        ImGui::SeparatorText("Style");
        if (node->type != UiNodeType::text) {
            draw_style_editor(*node);
        } else {
            ImGui::TextDisabled("(text nodes inherit style from their parent)");
        }

        // Material — only nodes with a material="..." attribute have
        // one (img/div slots typically). Same param widget the 3D scene
        // properties panel uses for mesh materials.
        if (node->attributes.contains("material")) {
            draw_node_material_editor(*node);
        }
    }
    ImGui::End();
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

    ImGui::SeparatorText("Material");

    auto* pipeline_cache = deps_.vulkan_context->pipeline_cache();
    if (!pipeline_cache) return;
    uint32_t pid = mat->pipeline_id();
    if (pid >= pipeline_cache->shader_count()) return;

    const auto& meta = pipeline_cache->metadata(pid);
    ImGui::TextDisabled("Shader: %s", mat->shader_path().c_str());

    // Param values live in the bindless SSBO and the fragment shader
    // samples them fresh every draw — but GameUi::update() stops issuing
    // new draw commands after a few idle frames (MAX_FRAMES_IN_FLIGHT
    // redraw countdown, see game_ui.cpp) once nothing marks the UI
    // dirty. A param edit doesn't touch the .fjui/.fjss tree so nothing
    // else would ever trip that flag — without mark_dirty() here, the
    // change lands in the SSBO but the preview simply stops re-drawing
    // before it ever samples it.
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

void UiEditorContext::style_edit_float(UiNode& node, const char* property, const char* label,
                                       float resolved_value, float speed, float min, float max) {
    // Empty explicit_str means "no rule sets this yet" — kept empty (not
    // defaulted to the resolved value's string form) so an undo restores
    // that exact unset state via SetStylePropertyCommand's empty-value
    // erase, rather than pinning the resolved value as a new explicit rule.
    auto explicit_str = style_prop_string(node, property, {});
    float source_value = resolved_value;
    if (!explicit_str.empty()) {
        try { source_value = std::stof(explicit_str); } catch (...) {}
    }

    // While a drag gesture is active, the widget must keep showing (and
    // ImGui must keep accumulating into) the buffered in-progress value
    // — not source_value, which only reflects the last *committed* edit
    // and would otherwise reset the field back to its starting point on
    // literally the next frame of the same drag (mark_dirty()/resolve()
    // don't run until the gesture ends, so source_value never moves
    // mid-drag).
    ImGuiID id = ImGui::GetID(label);
    auto it = active_float_edits_.find(id);
    bool dragging = it != active_float_edits_.end();
    float edited = dragging ? it->second : source_value;

    ImGui::DragFloat(label, &edited, speed, min, max, "%.1f");
    style_origin_tooltip(property);

    if (ImGui::IsItemActive()) {
        active_float_edits_[id] = edited;
    } else if (dragging) {
        active_float_edits_.erase(it);
    }

    if (ImGui::IsItemDeactivatedAfterEdit() && edited != source_value) {
        command_history().execute(std::make_unique<SetStylePropertyCommand>(
            *preview_ui_, node, property, explicit_str, to_css(UiLength::pixels(edited)),
            std::string("Set ") + property));
    }
}

void UiEditorContext::style_edit_length(UiNode& node, const char* property,
                                        UiLength resolved_value, float speed, float max) {
    const auto* desc = find_ui_style_property(property);
    if (!desc) return;

    // Same empty-means-unset convention as style_edit_float: the explicit
    // string is what undo restores to.
    auto explicit_str = style_prop_string(node, property, {});
    UiLength source = explicit_str.empty() ? resolved_value
                                           : StyleResolver::parse_length(explicit_str);

    auto commit = [&](UiLength length) {
        command_history().execute(std::make_unique<SetStylePropertyCommand>(
            *preview_ui_, node, property, explicit_str, to_css(length),
            std::string("Set ") + property));
    };

    ImGui::PushID(property);
    if (source.is_auto()) {
        // No number to drag; the button reads as the value itself and a
        // click starts a pixel value from whatever layout resolved.
        if (ImGui::Button("auto", ImVec2(ImGui::CalcItemWidth(), 0.0f))) {
            commit(UiLength::pixels(0.0f));
        }
        style_origin_tooltip(property);
        ImGui::SameLine();
        ImGui::TextUnformatted(property);
    } else {
        ImGuiID id = ImGui::GetID(property);
        auto it = active_float_edits_.find(id);
        bool dragging = it != active_float_edits_.end();
        float edited = dragging ? it->second : source.value;

        std::string format = source.unit == UiUnit::px ? "%.1f px"
                           : source.is_percent() ? "%.1f %%"
                           : std::string("%.2f ") + unit_suffix(source.unit);
        float min = 0.0f;
        float limit = source.is_percent() ? 100.0f : source.is_relative() ? max / 16.0f : max;
        // Insets and margins may be negative; percentages of them too.
        if (desc->auto_allowed) min = -limit;
        float step = source.is_relative() ? speed / 16.0f : speed;
        ImGui::DragFloat(property, &edited, step, min, limit, format.c_str());
        style_origin_tooltip(property);

        if (ImGui::IsItemActive()) {
            active_float_edits_[id] = edited;
        } else if (dragging) {
            active_float_edits_.erase(it);
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && edited != source.value) {
            commit({edited, source.unit});
        }
    }

    // Unit switch on the item just drawn, whichever it was.
    if (ImGui::BeginPopupContextItem("unit")) {
        float value = source.is_auto() ? 0.0f : source.value;
        if (ImGui::MenuItem("px", nullptr, source.unit == UiUnit::px)) {
            commit(UiLength::pixels(value));
        }
        if (ImGui::MenuItem("%", nullptr, source.is_percent())) {
            commit(UiLength::percent(value));
        }
        for (UiUnit unit : {UiUnit::em, UiUnit::rem, UiUnit::vw, UiUnit::vh}) {
            if (ImGui::MenuItem(unit_suffix(unit), nullptr, source.unit == unit)) {
                commit({value, unit});
            }
        }
        if (desc->auto_allowed && ImGui::MenuItem("auto", nullptr, source.is_auto())) {
            commit(UiLength::automatic());
        }
        if (!explicit_str.empty()) {
            ImGui::Separator();
            if (ImGui::MenuItem("Unset")) {
                command_history().execute(std::make_unique<SetStylePropertyCommand>(
                    *preview_ui_, node, property, explicit_str, std::string{},
                    std::string("Unset ") + property));
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void UiEditorContext::style_edit_color(UiNode& node, const char* property, const char* label,
                                       const UiColor& resolved_value) {
    auto explicit_str = style_prop_string(node, property, {});
    UiColor source = explicit_str.empty() ? resolved_value : StyleResolver::parse_color(explicit_str);

    // Same buffered-while-active pattern as style_edit_float — see there.
    ImGuiID id = ImGui::GetID(label);
    auto it = active_color_edits_.find(id);
    bool dragging = it != active_color_edits_.end();
    std::array<float, 4> rgba = dragging
        ? it->second : std::array{source.r, source.g, source.b, source.a};

    ImGui::ColorEdit4(label, rgba.data(), ImGuiColorEditFlags_AlphaBar);
    style_origin_tooltip(property);

    if (ImGui::IsItemActive()) {
        active_color_edits_[id] = rgba;
    } else if (dragging) {
        active_color_edits_.erase(it);
    }

    if (ImGui::IsItemDeactivatedAfterEdit()) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x",
                      static_cast<int>(rgba[0] * 255), static_cast<int>(rgba[1] * 255),
                      static_cast<int>(rgba[2] * 255), static_cast<int>(rgba[3] * 255));
        std::string new_str = buf;
        if (new_str != explicit_str) {
            command_history().execute(std::make_unique<SetStylePropertyCommand>(
                *preview_ui_, node, property, explicit_str, new_str,
                std::string("Set ") + property));
        }
    }
}

void UiEditorContext::style_edit_keyword(UiNode& node, const char* property) {
    const auto* desc = find_ui_style_property(property);
    if (!desc || desc->kind != UiPropertyKind::keyword) return;

    // The keyword table's entries are string literals, so data() is a
    // null-terminated string ImGui can list directly.
    std::vector<const char*> items;
    for (auto keyword : desc->keywords) items.push_back(keyword.data());

    int index = desc->get_keyword(node.style);
    auto explicit_str = style_prop_string(node, property, {});
    if (!explicit_str.empty()) {
        for (size_t i = 0; i < items.size(); ++i) {
            if (explicit_str == items[i]) { index = static_cast<int>(i); break; }
        }
    }

    int edited = index;
    ImGui::Combo(property, &edited, items.data(), static_cast<int>(items.size()));
    style_origin_tooltip(property);

    if (edited != index) {
        // Same empty-means-unset convention as style_edit_float/color:
        // explicit_str (not a stringified resolved fallback) is what
        // undo restores to.
        command_history().execute(std::make_unique<SetStylePropertyCommand>(
            *preview_ui_, node, property, explicit_str, items[edited],
            std::string("Set ") + property));
    }
}

void UiEditorContext::style_origin_tooltip(const char* property) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) return;
    auto it = style_origins_.find(property);
    if (it == style_origins_.end()) {
        const auto* desc = find_ui_style_property(property);
        ImGui::SetTooltip("%s", desc && desc->inherited ? "inherited from the parent (no rule sets it here)"
                                                        : "default (no rule sets it)");
        return;
    }
    const auto& origin = it->second;
    if (origin.inline_style) {
        ImGui::SetTooltip("style-%s attribute on this element", property);
        return;
    }
    auto file = fs::path(origin.rule->source).filename().string();
    if (file.empty()) file = "(unsaved rule)";
    ImGui::SetTooltip("%s: %s\n%s { %s: %s; }", file.c_str(), to_css(origin.rule->selector).c_str(),
                      to_css(origin.rule->selector).c_str(), property, origin.declaration->value.c_str());
}

void UiEditorContext::draw_style_editor(UiNode& node) {
    auto& s = node.style;
    style_origins_ = preview_ui_ ? StyleResolver::explain(node, preview_ui_->stylesheet())
                                 : std::unordered_map<std::string, StyleOrigin>{};

    if (ImGui::TreeNodeEx("Layout", ImGuiTreeNodeFlags_DefaultOpen)) {
        style_edit_keyword(node, "display");
        style_edit_keyword(node, "flex-direction");
        style_edit_keyword(node, "justify-content");
        style_edit_keyword(node, "align-items");
        style_edit_keyword(node, "align-self");
        style_edit_keyword(node, "flex-wrap");
        style_edit_keyword(node, "position");
        style_edit_keyword(node, "overflow");
        style_edit_float(node, "flex-grow", "flex-grow", s.flex_grow, 0.1f, 0.0f, 100.0f);
        style_edit_float(node, "flex-shrink", "flex-shrink", s.flex_shrink, 0.1f, 0.0f, 100.0f);
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Dimensions", ImGuiTreeNodeFlags_DefaultOpen)) {
        style_edit_length(node, "width", s.width, 1.0f, 4096.0f);
        style_edit_length(node, "height", s.height, 1.0f, 4096.0f);
        style_edit_length(node, "min-width", s.min_width, 1.0f, 4096.0f);
        style_edit_length(node, "min-height", s.min_height, 1.0f, 4096.0f);
        style_edit_length(node, "max-width", s.max_width, 1.0f, 4096.0f);
        style_edit_length(node, "max-height", s.max_height, 1.0f, 4096.0f);
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Spacing", ImGuiTreeNodeFlags_DefaultOpen)) {
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
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Visual", ImGuiTreeNodeFlags_DefaultOpen)) {
        style_edit_color(node, "background-color", "background-color", s.background_color);
        style_edit_color(node, "color", "color", s.color);
        style_edit_float(node, "font-size", "font-size", s.font_size, 0.5f, 1.0f, 256.0f);
        style_edit_color(node, "border-color", "border-color", s.border_color);
        style_edit_float(node, "border-width", "border-width", s.border_width, 0.5f, 0.0f, 64.0f);
        style_edit_float(node, "border-radius", "border-radius",
                         s.border_radius.top_left, 0.5f, 0.0f, 256.0f);
        style_edit_float(node, "opacity", "opacity", s.opacity, 0.01f, 0.0f, 1.0f);
        ImGui::TreePop();
    }
}

// ── Mock Data Panel ──────────────────────────────────────────────────────

void UiEditorContext::draw_mock_data() {
    if (ImGui::Begin(ctx_title("Mock Data").c_str())) {
        if (!preview_ui_ || !preview_ui_->loaded()) {
            ImGui::TextDisabled("No .fjui loaded");
            ImGui::End();
            return;
        }

        if (is_widget_file_) {
            ImGui::TextColored({0.6f, 0.45f, 0.8f, 1.0f}, "Widget isolation mode");
            if (!loaded_widget_.props.empty()) {
                ImGui::TextDisabled("Props from <prop> declarations");
            } else {
                ImGui::TextDisabled("Props auto-discovered from {key} templates");
            }
        } else {
            ImGui::TextDisabled("Key/value pairs for {key} data binding");
        }
        ImGui::Separator();

        // Existing entries
        int remove_idx = -1;
        for (int i = 0; i < static_cast<int>(mock_entries_.size()); ++i) {
            ImGui::PushID(i);

            auto& [key, value] = mock_entries_[i];

            // Key label
            ImGui::Text("%s", key.c_str());
            ImGui::SameLine(150);

            // Find prop type if declared
            std::string prop_type;
            for (const auto& prop : loaded_widget_.props) {
                if (prop.name == key) { prop_type = prop.type; break; }
            }

            ImGui::SetNextItemWidth(-40);
            bool changed = false;

            if (prop_type == "float") {
                float f = 0.0f;
                try { f = std::stof(value); } catch (...) {}
                if (ImGui::DragFloat("##val", &f, 0.1f)) {
                    value = std::to_string(f);
                    preview_ui_->context().set_float(key, f);
                    changed = true;
                }
            } else if (prop_type == "int") {
                int iv = 0;
                try { iv = std::stoi(value); } catch (...) {}
                if (ImGui::DragInt("##val", &iv)) {
                    value = std::to_string(iv);
                    preview_ui_->context().set_int(key, iv);
                    changed = true;
                }
            } else if (prop_type == "bool") {
                bool bv = (value != "0" && value != "false");
                if (ImGui::Checkbox("##val", &bv)) {
                    value = bv ? "1" : "0";
                    preview_ui_->context().set_bool(key, bv);
                    changed = true;
                }
            } else {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", value.c_str());
                if (ImGui::InputText("##val", buf, sizeof(buf))) {
                    value = buf;
                    preview_ui_->context().set(key, value);
                    changed = true;
                }
            }

            if (changed) {
                preview_ui_->mark_dirty();
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                remove_idx = i;
            }

            ImGui::PopID();
        }

        if (remove_idx >= 0) {
            mock_entries_.erase(mock_entries_.begin() + remove_idx);
            // Reload UI to clear the removed binding
            if (!fjui_path_.empty()) {
                auto abs_path = base_path::resolve_asset(fjui_path_, deps_.project_mgr->active());
                preview_ui_->load(abs_path, fjss_path_);
                for (const auto& [key, value] : mock_entries_) {
                    preview_ui_->context().set(key, value);
                }
            }
        }

        // Add new entry
        ImGui::Separator();
        ImGui::SetNextItemWidth(140);
        ImGui::InputTextWithHint("##newkey", "key", new_key_buf_, sizeof(new_key_buf_));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-60);
        ImGui::InputTextWithHint("##newval", "value", new_value_buf_, sizeof(new_value_buf_));
        ImGui::SameLine();
        if (ImGui::SmallButton("Add") && new_key_buf_[0] != '\0') {
            mock_entries_.emplace_back(new_key_buf_, new_value_buf_);
            preview_ui_->context().set(new_key_buf_, new_value_buf_);
            preview_ui_->mark_dirty();
            new_key_buf_[0] = '\0';
            new_value_buf_[0] = '\0';
        }

        // ── List Data (for foreach) ─────────────────────────────────
        ImGui::Spacing();
        ImGui::SeparatorText("Lists (foreach)");
        ImGui::TextDisabled("List data for foreach directives");

        bool lists_changed = false;
        int remove_list = -1;
        for (int li = 0; li < static_cast<int>(mock_lists_.size()); ++li) {
            ImGui::PushID(2000 + li);
            auto& list = mock_lists_[li];

            bool list_open = ImGui::TreeNodeEx(list.key.c_str(),
                ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::SameLine();
            if (ImGui::SmallButton("X##list")) { remove_list = li; }
            ImGui::SameLine();
            if (ImGui::SmallButton("+Item")) {
                list.items.push_back({});
                lists_changed = true;
            }

            if (list_open) {
                int remove_item = -1;
                for (int ii = 0; ii < static_cast<int>(list.items.size()); ++ii) {
                    ImGui::PushID(ii);
                    auto& item = list.items[ii];

                    ImGui::TextDisabled("[%d]", ii);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X##item")) { remove_item = ii; }

                    // Existing fields
                    for (auto& [fk, fv] : item.fields) {
                        ImGui::Indent();
                        ImGui::Text("%s", fk.c_str());
                        ImGui::SameLine(150);
                        char fbuf[256];
                        std::snprintf(fbuf, sizeof(fbuf), "%s", fv.c_str());
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputText("##fval", fbuf, sizeof(fbuf))) {
                            fv = fbuf;
                            lists_changed = true;
                        }
                        ImGui::Unindent();
                    }

                    // Add field button
                    ImGui::Indent();
                    static char field_key[64]{};
                    static char field_val[128]{};
                    ImGui::SetNextItemWidth(80);
                    ImGui::InputTextWithHint("##fk", "key", field_key, sizeof(field_key));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80);
                    ImGui::InputTextWithHint("##fv", "value", field_val, sizeof(field_val));
                    ImGui::SameLine();
                    if (ImGui::SmallButton("+") && field_key[0] != '\0') {
                        item.fields.emplace_back(field_key, field_val);
                        field_key[0] = '\0';
                        field_val[0] = '\0';
                        lists_changed = true;
                    }
                    ImGui::Unindent();

                    ImGui::PopID();
                }
                if (remove_item >= 0) {
                    list.items.erase(list.items.begin() + remove_item);
                    lists_changed = true;
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (remove_list >= 0) {
            mock_lists_.erase(mock_lists_.begin() + remove_list);
            lists_changed = true;
        }

        // Add new list
        ImGui::SetNextItemWidth(120);
        ImGui::InputTextWithHint("##newlistkey", "list key", new_list_key_buf_, sizeof(new_list_key_buf_));
        ImGui::SameLine();
        if (ImGui::SmallButton("Add List") && new_list_key_buf_[0] != '\0') {
            mock_lists_.push_back({new_list_key_buf_, {}});
            new_list_key_buf_[0] = '\0';
            lists_changed = true;
        }

        if (lists_changed) {
            sync_mock_lists();
        }

        // ── Action Bindings ──────────────────────────────────────────
        ImGui::Spacing();
        ImGui::SeparatorText("Action Bindings");
        ImGui::TextDisabled("on:click action -> set key = value");

        int remove_bind = -1;
        for (int i = 0; i < static_cast<int>(action_bindings_.size()); ++i) {
            ImGui::PushID(1000 + i);
            auto& b = action_bindings_[i];
            ImGui::Text("%s", b.action.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("->");
            ImGui::SameLine();
            ImGui::Text("%s = %s", b.key.c_str(), b.value.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                remove_bind = i;
            }
            ImGui::PopID();
        }
        if (remove_bind >= 0) {
            action_bindings_.erase(action_bindings_.begin() + remove_bind);
        }

        // Add new binding — action from dropdown, key/value as text
        if (!discovered_actions_.empty()) {
            ImGui::SetNextItemWidth(120);
            if (ImGui::BeginCombo("##bact",
                    bind_action_idx_ < static_cast<int>(discovered_actions_.size())
                        ? discovered_actions_[bind_action_idx_].c_str() : "")) {
                for (int i = 0; i < static_cast<int>(discovered_actions_.size()); ++i) {
                    bool selected = (i == bind_action_idx_);
                    if (ImGui::Selectable(discovered_actions_[i].c_str(), selected)) {
                        bind_action_idx_ = i;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("->");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::InputTextWithHint("##bkey", "key", bind_key_buf_, sizeof(bind_key_buf_));
            ImGui::SameLine();
            ImGui::Text("=");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::InputTextWithHint("##bval", "value", bind_value_buf_, sizeof(bind_value_buf_));
            ImGui::SameLine();
            if (ImGui::SmallButton("Bind") && bind_key_buf_[0] != '\0') {
                action_bindings_.push_back({
                    discovered_actions_[bind_action_idx_],
                    bind_key_buf_, bind_value_buf_});
                bind_key_buf_[0] = '\0';
                bind_value_buf_[0] = '\0';
            }
        } else {
            ImGui::TextDisabled("No on:click actions found in .fjui");
        }
    }
    ImGui::End();
}

// ── List sync ────────────────────────────────────────────────────────────

void UiEditorContext::sync_mock_lists() {
    if (!preview_ui_) return;
    auto& ctx = preview_ui_->context();

    for (const auto& ml : mock_lists_) {
        std::vector<UiContext> items;
        items.reserve(ml.items.size());
        for (const auto& item : ml.items) {
            UiContext item_ctx;
            for (const auto& [k, v] : item.fields) {
                item_ctx.set(k, v);
            }
            items.push_back(std::move(item_ctx));
        }
        ctx.set_list(ml.key, std::move(items));
    }
    preview_ui_->mark_dirty();
}

// ── Action Log Panel ─────────────────────────────────────────────────────

void UiEditorContext::draw_action_log() {
    if (ImGui::Begin(ctx_title("Action Log").c_str())) {
        if (ImGui::SmallButton("Clear")) {
            action_log_.clear();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%d actions)", static_cast<int>(action_log_.size()));
        ImGui::Separator();

        for (auto it = action_log_.rbegin(); it != action_log_.rend(); ++it) {
            ImGui::TextDisabled("[%.1fs]", it->timestamp);
            ImGui::SameLine();
            ImGui::Text("%s", it->action.c_str());
        }
    }
    ImGui::End();
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

void UiEditorContext::draw_layout_overlay(UiNode* node, ImDrawList* draw_list,
                                           ImVec2 origin, float scale) {
    if (node->hidden_by_if) return;
    if (node->style.display == UiDisplay::none) return;

    auto& r = node->layout_rect;
    if (r.w < 0.5f && r.h < 0.5f) {
        for (auto& child : node->children) {
            draw_layout_overlay(child.get(), draw_list, origin, scale);
        }
        return;
    }

    ImVec2 p0 = {origin.x + r.x * scale, origin.y + r.y * scale};
    ImVec2 p1 = {p0.x + r.w * scale, p0.y + r.h * scale};

    // Margin area (orange, semi-transparent fill)
    auto& m = node->layout_margin;
    if (m.top > 0 || m.right > 0 || m.bottom > 0 || m.left > 0) {
        ImVec2 mp0 = {p0.x - m.left * scale, p0.y - m.top * scale};
        ImVec2 mp1 = {p1.x + m.right * scale, p1.y + m.bottom * scale};
        // Top margin strip
        draw_list->AddRectFilled(mp0, {mp1.x, p0.y}, IM_COL32(255, 165, 0, 40));
        // Bottom margin strip
        draw_list->AddRectFilled({mp0.x, p1.y}, mp1, IM_COL32(255, 165, 0, 40));
        // Left margin strip
        draw_list->AddRectFilled({mp0.x, p0.y}, {p0.x, p1.y}, IM_COL32(255, 165, 0, 40));
        // Right margin strip
        draw_list->AddRectFilled({p1.x, p0.y}, {mp1.x, p1.y}, IM_COL32(255, 165, 0, 40));
    }

    // Padding area (green, semi-transparent fill)
    auto& pd = node->layout_padding;
    if (pd.top > 0 || pd.right > 0 || pd.bottom > 0 || pd.left > 0) {
        ImVec2 pp0 = {p0.x + pd.left * scale, p0.y + pd.top * scale};
        ImVec2 pp1 = {p1.x - pd.right * scale, p1.y - pd.bottom * scale};
        // Top padding strip
        draw_list->AddRectFilled(p0, {p1.x, pp0.y}, IM_COL32(100, 200, 100, 40));
        // Bottom padding strip
        draw_list->AddRectFilled({p0.x, pp1.y}, p1, IM_COL32(100, 200, 100, 40));
        // Left padding strip
        draw_list->AddRectFilled({p0.x, pp0.y}, {pp0.x, pp1.y}, IM_COL32(100, 200, 100, 40));
        // Right padding strip
        draw_list->AddRectFilled({pp1.x, pp0.y}, {p1.x, pp1.y}, IM_COL32(100, 200, 100, 40));
    }

    // Content border (blue outline)
    draw_list->AddRect(p0, p1, IM_COL32(100, 150, 255, 80));

    // Children
    for (auto& child : node->children) {
        draw_layout_overlay(child.get(), draw_list, origin, scale);
    }
}

} // namespace fjell
