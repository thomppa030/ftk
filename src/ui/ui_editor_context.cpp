#include "ui/ui_editor_context.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "renderer/texture.hpp"
#include "core/base_path.hpp"
#include "core/clock.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/code_editor.hpp"
#include "ui/icon_cache.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fjell {

namespace fs = std::filesystem;

UiEditorContext::UiEditorContext(UiEditorDeps deps)
    : deps_{deps} {}

void UiEditorContext::init() {
    // Preview GameUi is initialized lazily when a .fjui file is opened
}

void UiEditorContext::shutdown() {
    // Clear the UiContext's action delegates before disconnecting connections.
    // Disconnecting via swap-and-pop can destroy other listeners' callbacks,
    // causing use-after-free when the next connection tries to disconnect.
    if (preview_ui_) {
        preview_ui_->context().clear();
    }
    action_connections_.clear();
    selected_node_ = nullptr;
    if (preview_ui_) {
        preview_ui_->destroy();
        preview_ui_.reset();
    }
}

void UiEditorContext::draw(float dt) {
    elapsed_time_ += dt;
    draw_preview(dt);
    draw_element_tree();
    draw_inspector();
    draw_mock_data();
    draw_action_log();
}

void UiEditorContext::setup_dockspace(ImGuiID main_area) {
    ImGuiID center = main_area;

    // Left panel: Element Tree — 16%
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.16f, nullptr, &center);

    // Right panel: Inspector (top) + Mock Data (bottom) — 24%
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    ImGuiID right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);

    // Bottom: Action Log + Stats/Console — 25%
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);

    int ci = context_index();
    char stats[32], console[32], history[32], code[32];
    std::snprintf(stats, sizeof(stats), "Stats##ctx%d", ci);
    std::snprintf(console, sizeof(console), "Console##ctx%d", ci);
    std::snprintf(history, sizeof(history), "History##ctx%d", ci);
    std::snprintf(code, sizeof(code), "Code Editor##ctx%d", ci);

    ImGui::DockBuilderDockWindow("Element Tree", left);
    ImGui::DockBuilderDockWindow(code, center);
    ImGui::DockBuilderDockWindow("UI Preview", center);
    ImGui::DockBuilderDockWindow("UI Inspector", right);
    ImGui::DockBuilderDockWindow("Mock Data", right_bottom);
    ImGui::DockBuilderDockWindow("Action Log", bottom);
    ImGui::DockBuilderDockWindow(history, bottom);
    ImGui::DockBuilderDockWindow(stats, bottom);
    ImGui::DockBuilderDockWindow(console, bottom);
}

bool UiEditorContext::handle_shortcuts() {
    if (ImGui::GetIO().WantTextInput) return false;

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

void UiEditorContext::open_ui(const std::string& fjui_path) {
    fjui_path_ = fjui_path;

    auto abs_path = base_path::resolve_asset(fjui_path, deps_.project_mgr->active());

    // Derive .fjss path: same name, .fjss extension
    auto fjss = fs::path(abs_path);
    fjss.replace_extension(".fjss");
    fjss_path_ = fjss.string();

    // Set tab name to filename
    name_ = fs::path(fjui_path).stem().string();

    // Initialize preview GameUi if needed
    if (!preview_ui_) {
        preview_ui_ = std::make_unique<GameUi>();
        if (deps_.engine_game_ui) {
            // Share font atlas from the engine's GameUi (avoids ~1s msdfgen rebuild)
            deps_.vulkan_context->init_game_ui_shared(*preview_ui_,
                                                       deps_.engine_game_ui->font());
        } else {
            deps_.vulkan_context->init_game_ui(*preview_ui_,
                base_path::resolve("engine_assets/fonts/Inter-Regular.ttf"));
        }
        preview_ui_->set_resource_manager(deps_.resource_manager);
    }

    // Load the .fjui + .fjss
    preview_ui_->load(abs_path, fjss_path_);

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

    // Open the .fjui source in the code editor
    if (deps_.code_editor) {
        deps_.code_editor->open(abs_path);
    }

    selected_node_ = nullptr;
    FJELL_CORE_INFO("UI editor opened: {}", fjui_path);
}

// ── Preview Panel ────────────────────────────────────────────────────────

void UiEditorContext::draw_preview(float dt) {
    if (ImGui::Begin("UI Preview")) {
        if (!preview_ui_ || !preview_ui_->loaded()) {
            ImGui::TextDisabled("No .fjui loaded");
            ImGui::End();
            return;
        }

        // Resolution controls
        float prev_w = preview_width_;
        float prev_h = preview_height_;
        ImGui::Text("Resolution:");
        ImGui::SameLine();
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

        // Resolution changed — force layout recompute
        if (preview_width_ != prev_w || preview_height_ != prev_h) {
            preview_ui_->mark_dirty();
        }

        ImGui::Separator();

        // Draw preview area — render layout rects using ImGui DrawList
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.x < 1.0f || avail.y < 1.0f) {
            ImGui::End();
            return;
        }

        // Re-apply mock data each frame (ensures values persist after hot-reload)
        for (const auto& [key, value] : mock_entries_) {
            preview_ui_->context().set(key, value);
        }

        // Update the preview UI (layout, data bind, etc.)
        preview_ui_->update(preview_width_, preview_height_, 0, dt);

        // Scale to fit available area
        float scale_x = avail.x / preview_width_;
        float scale_y = avail.y / preview_height_;
        float scale = std::min(scale_x, scale_y);

        float scaled_w = preview_width_ * scale;
        float scaled_h = preview_height_ * scale;

        // Center in available area
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        float offset_x = (avail.x - scaled_w) * 0.5f;
        float offset_y = (avail.y - scaled_h) * 0.5f;
        ImVec2 origin = {cursor.x + offset_x, cursor.y + offset_y};

        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        // Dark background
        draw_list->AddRectFilled(origin, {origin.x + scaled_w, origin.y + scaled_h},
                                  IM_COL32(24, 24, 28, 255));
        draw_list->AddRect(origin, {origin.x + scaled_w, origin.y + scaled_h},
                           IM_COL32(60, 60, 68, 255));

        // Draw UI nodes as colored rectangles
        auto* root = preview_ui_->root();
        if (root) {
            draw_node_preview(root, draw_list, origin, scale);

            // Edit mode: layout debug overlay (drawn on top of everything)
            if (!play_mode_ && show_layout_overlay_) {
                draw_layout_overlay(root, draw_list, origin, scale);
            }
        }

        // Invisible button for interaction
        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##preview_area", {scaled_w, scaled_h});

        if (play_mode_) {
            // Play mode: forward mouse events to GameUi for interaction
            if (root) {
                process_play_input(root, origin, scale);
            }
        } else {
            // Edit mode: click-to-select elements
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ImVec2 mouse = ImGui::GetMousePos();
                float local_x = (mouse.x - origin.x) / scale;
                float local_y = (mouse.y - origin.y) / scale;

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
                origin.x + (lr.x + layout_node->style.padding.left) * scale,
                origin.y + (lr.y + layout_node->style.padding.top) * scale
            };
            draw_list->AddText(nullptr, font_size, text_pos, text_col,
                               node->text_content.c_str());
        }
    }

    // Highlight selected node
    if (node == selected_node_) {
        draw_list->AddRect(p0, p1, IM_COL32(100, 180, 255, 200), 0, 0, 2.0f);

        // Margin overlay (orange)
        auto& m = node->style.margin;
        if (m.top > 0 || m.right > 0 || m.bottom > 0 || m.left > 0) {
            ImVec2 mp0 = {p0.x - m.left * scale, p0.y - m.top * scale};
            ImVec2 mp1 = {p1.x + m.right * scale, p1.y + m.bottom * scale};
            draw_list->AddRect(mp0, mp1, IM_COL32(255, 165, 0, 120), 0, 0, 1.0f);
        }

        // Padding overlay (green)
        auto& pd = node->style.padding;
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
    if (ImGui::Begin("Element Tree")) {
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
    } else {
        label = "<" + (node->tag.empty() ? "root" : node->tag) + ">";
        if (!node->id.empty()) {
            label += " #" + node->id;
        }
        for (const auto& cls : node->classes) {
            label += " ." + cls;
        }
    }

    // Gray out hidden nodes
    if (node->hidden_by_if || node->style.display == UiDisplay::none) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }

    bool open = ImGui::TreeNodeEx(node, flags, "%s", label.c_str());

    if (node->hidden_by_if || node->style.display == UiDisplay::none) {
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

void UiEditorContext::draw_inspector() {
    if (ImGui::Begin("UI Inspector")) {
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
                if (!cls_str.empty()) cls_str += ", ";
                cls_str += "." + c;
            }
            ImGui::Text("Classes: %s", cls_str.c_str());
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

        // Attributes
        if (!node->attributes.empty()) {
            ImGui::SeparatorText("Attributes");
            for (const auto& [key, value] : node->attributes) {
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

            auto& m = node->style.margin;
            ImGui::Text("Margin: %.0f %.0f %.0f %.0f", m.top, m.right, m.bottom, m.left);
            auto& p = node->style.padding;
            ImGui::Text("Padding: %.0f %.0f %.0f %.0f", p.top, p.right, p.bottom, p.left);
        }

        // Computed style
        {
            ImGui::SeparatorText("Computed Style");
            auto& s = node->style;

            // Display & layout
            const char* display_str = s.display == UiDisplay::flex ? "flex" : "none";
            ImGui::Text("display: %s", display_str);

            const char* dir_strs[] = {"row", "column", "row-reverse", "column-reverse"};
            ImGui::Text("flex-direction: %s", dir_strs[static_cast<int>(s.flex_direction)]);

            const char* justify_strs[] = {"flex-start", "flex-end", "center",
                                          "space-between", "space-around", "space-evenly"};
            ImGui::Text("justify-content: %s", justify_strs[static_cast<int>(s.justify_content)]);

            const char* align_strs[] = {"auto", "flex-start", "flex-end",
                                        "center", "stretch", "baseline"};
            ImGui::Text("align-items: %s", align_strs[static_cast<int>(s.align_items)]);

            if (s.flex_grow > 0.0f) ImGui::Text("flex-grow: %.1f", s.flex_grow);
            if (s.flex_shrink != 1.0f) ImGui::Text("flex-shrink: %.1f", s.flex_shrink);

            // Dimensions
            if (s.width >= 0.0f) ImGui::Text("width: %.0f", s.width);
            if (s.height >= 0.0f) ImGui::Text("height: %.0f", s.height);

            // Visual
            ImGui::Text("background: rgba(%.0f, %.0f, %.0f, %.2f)",
                        s.background_color.r * 255, s.background_color.g * 255,
                        s.background_color.b * 255, s.background_color.a);
            ImGui::Text("color: rgba(%.0f, %.0f, %.0f, %.2f)",
                        s.color.r * 255, s.color.g * 255,
                        s.color.b * 255, s.color.a);
            ImGui::Text("font-size: %.0f", s.font_size);

            if (s.border_width > 0.0f) {
                ImGui::Text("border: %.0f solid rgba(%.0f,%.0f,%.0f,%.2f)",
                            s.border_width,
                            s.border_color.r * 255, s.border_color.g * 255,
                            s.border_color.b * 255, s.border_color.a);
            }
            if (s.border_radius.top_left > 0.0f || s.border_radius.top_right > 0.0f) {
                ImGui::Text("border-radius: %.0f %.0f %.0f %.0f",
                            s.border_radius.top_left, s.border_radius.top_right,
                            s.border_radius.bottom_right, s.border_radius.bottom_left);
            }
            if (s.opacity < 1.0f) ImGui::Text("opacity: %.2f", s.opacity);
            if (s.box_shadow.active()) {
                ImGui::Text("box-shadow: %.0f %.0f %.0f %.0f",
                            s.box_shadow.offset_x, s.box_shadow.offset_y,
                            s.box_shadow.blur, s.box_shadow.spread);
            }
            if (s.gradient.active()) {
                ImGui::Text("gradient: %.0f deg", s.gradient.angle);
            }
            if (!s.transition.empty()) {
                ImGui::Text("transition: %s", s.transition.c_str());
            }

            const char* overflow_strs[] = {"visible", "hidden", "scroll"};
            if (s.overflow != UiOverflow::visible) {
                ImGui::Text("overflow: %s", overflow_strs[static_cast<int>(s.overflow)]);
            }
        }
    }
    ImGui::End();
}

// ── Mock Data Panel ──────────────────────────────────────────────────────

void UiEditorContext::draw_mock_data() {
    if (ImGui::Begin("Mock Data")) {
        if (!preview_ui_ || !preview_ui_->loaded()) {
            ImGui::TextDisabled("No .fjui loaded");
            ImGui::End();
            return;
        }

        ImGui::TextDisabled("Key/value pairs for {key} data binding");
        ImGui::Separator();

        // Existing entries
        int remove_idx = -1;
        for (int i = 0; i < static_cast<int>(mock_entries_.size()); ++i) {
            ImGui::PushID(i);

            auto& [key, value] = mock_entries_[i];

            // Key label
            ImGui::Text("%s", key.c_str());
            ImGui::SameLine(150);

            // Editable value
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", value.c_str());
            ImGui::SetNextItemWidth(-40);
            if (ImGui::InputText("##val", buf, sizeof(buf))) {
                value = buf;
                preview_ui_->context().set(key, value);
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

// ── Action Log Panel ─────────────────────────────────────────────────────

void UiEditorContext::draw_action_log() {
    if (ImGui::Begin("Action Log")) {
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

    // Set hover on hit node AND all ancestors (CSS hover bubbles up)
    for (UiNode* n = hit; n != nullptr; n = n->parent) {
        n->interaction_state |= 1; // hovered
        preview_ui_->mark_dirty();
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
        // Set active on hit + ancestors
        for (UiNode* n = hit; n != nullptr; n = n->parent) {
            n->interaction_state |= 2; // active
        }
        preview_ui_->mark_dirty();
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
    auto& m = node->style.margin;
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
    auto& pd = node->style.padding;
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
