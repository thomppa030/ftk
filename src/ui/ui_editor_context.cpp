#include "ui/ui_editor_context.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/widget_asset.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "scripting/script_manager.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/texture.hpp"
#include "core/base_path.hpp"
#include "core/clock.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
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

void UiEditorContext::draw(float dt) {
    elapsed_time_ += dt;
    draw_preview(dt);
    draw_element_tree();
    draw_inspector();
    draw_mock_data();
    draw_action_log();
}

std::vector<std::string> UiEditorContext::docked_window_names() const {
    return {ctx_title("Element Tree"), ctx_title("UI Preview"),
            ctx_title("UI Inspector"), ctx_title("Mock Data"), ctx_title("Action Log"),
            ctx_title("History"), ctx_title("Stats"), ctx_title("Console")};
}

void UiEditorContext::setup_dockspace(ImGuiID main_area) {
    ImGuiID center = main_area;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.16f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    ImGuiID right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);

    auto names = docked_window_names();
    ImGui::DockBuilderDockWindow(names[0].c_str(), left);          // Element Tree
    ImGui::DockBuilderDockWindow(names[1].c_str(), center);        // UI Preview
    ImGui::DockBuilderDockWindow(names[2].c_str(), right);         // UI Inspector
    ImGui::DockBuilderDockWindow(names[3].c_str(), right_bottom);  // Mock Data
    ImGui::DockBuilderDockWindow(names[4].c_str(), bottom);        // Action Log
    ImGui::DockBuilderDockWindow(names[5].c_str(), bottom);        // History
    ImGui::DockBuilderDockWindow(names[6].c_str(), bottom);        // Stats
    ImGui::DockBuilderDockWindow(names[7].c_str(), bottom);        // Console
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

void UiEditorContext::open_ui(const std::string& path) {
    auto abs_path = base_path::resolve_asset(path, deps_.project_mgr->active());

    // If opening a .fjwidget, read the manifest to get .fjui/.fjss paths
    if (path.ends_with(".fjwidget")) {
        auto result = load_widget_asset(abs_path);
        if (result) {
            fjui_path_ = result->fjui_path;
            abs_path = base_path::resolve_asset(fjui_path_, deps_.project_mgr->active());
            if (!result->fjss_path.empty()) {
                fjss_path_ = base_path::resolve_asset(result->fjss_path, deps_.project_mgr->active());
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

        // Create offscreen passes for pixel-perfect preview (one per frame in flight)
        offscreen_width_ = 800;
        offscreen_height_ = 600;
        for (uint32_t i = 0; i < PREVIEW_FRAMES; ++i) {
            offscreen_[i] = std::make_unique<OffscreenPass>(
                deps_.vulkan_context->device_ref(), 800, 600, VK_SAMPLE_COUNT_1_BIT);
        }

        // Register preview callback — renders to the current frame's offscreen pass
        deps_.vulkan_context->set_ui_preview_callback(
            [this](uint32_t frame_index) {
                if (!preview_ui_ || !preview_ui_->loaded()) return;
                auto& target = offscreen_[frame_index % PREVIEW_FRAMES];
                if (!target) return;
                deps_.vulkan_context->record_game_ui_offscreen(
                    *preview_ui_, *target, frame_index);
            });
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

    // Detect widget isolation mode (file is in a widgets/ directory)
    is_widget_file_ = (fjui_path_.find("/widgets/") != std::string::npos ||
                       fjui_path_.find("\\widgets\\") != std::string::npos);

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
        preview_ui_->update(preview_width_, preview_height_, 0, dt, input_ptr);

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

        // Display offscreen render as ImGui image
        // Show the most recently completed frame's pass
        {
            // Use frame_index 0 as a simple read target — the callback writes
            // to frame_index % PREVIEW_FRAMES, and we read whichever has a valid texture.
            // With 3 frames in flight, at least one is always fully rendered.
            VkDescriptorSet tex = VK_NULL_HANDLE;
            for (uint32_t i = 0; i < PREVIEW_FRAMES; ++i) {
                if (offscreen_[i]) {
                    tex = offscreen_[i]->imgui_texture();
                    if (tex) break;
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
            draw_layout_overlay(root, draw_list, origin, scale);
        }

        // Invisible button for interaction (placed over the image)
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

void UiEditorContext::draw_inspector() {
    if (ImGui::Begin(ctx_title("UI Inspector").c_str())) {
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
    if (ImGui::Begin(ctx_title("Mock Data").c_str())) {
        if (!preview_ui_ || !preview_ui_->loaded()) {
            ImGui::TextDisabled("No .fjui loaded");
            ImGui::End();
            return;
        }

        if (is_widget_file_) {
            ImGui::TextColored({0.6f, 0.45f, 0.8f, 1.0f}, "Widget isolation mode");
            ImGui::TextDisabled("Props auto-discovered from {key} templates");
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
