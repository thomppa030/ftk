#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/fjui_parser.hpp"
#include "renderer/bindless.hpp"
#include "renderer/device.hpp"
#include "renderer/material.hpp"
#include "renderer/resource_manager.hpp"
#include "core/base_path.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fjell {

void GameUi::create(Device& device, const PassResources& resources,
                    BindlessTextures& bindless,
                    VkCommandPool command_pool, VkQueue queue,
                    const std::string& font_path,
                    VkFormat output_format) {
    bindless_ = &bindless;
    renderer_.create(device, resources, output_format);

    if (!font_path.empty() && std::filesystem::exists(font_path)) {
        font_atlas_.create(font_path, device.handle(), device.physical_device(),
                           command_pool, queue, bindless);
    }
}

void GameUi::set_resource_manager(ResourceManager* rm) {
    resource_manager_ = rm;
}

bool GameUi::needs_redraw() const {
    return needs_redraw_;
}

void GameUi::mark_dirty() {
    dirty_ = true;
    needs_redraw_ = true;
}

void GameUi::destroy() {
    font_atlas_.destroy();
    renderer_.destroy();
}

void GameUi::load(const std::string& fjui_path, const std::string& fjss_path) {
    if (!std::filesystem::exists(fjui_path)) {
        FJELL_CORE_WARN("GameUi: .fjui file not found: {}", fjui_path);
        return;
    }

    std::ifstream fjui_file(fjui_path);
    std::ostringstream fjui_ss;
    fjui_ss << fjui_file.rdbuf();

    root_ = FjuiParser::parse(fjui_ss.str());
    if (!root_) {
        FJELL_CORE_ERROR("GameUi: failed to parse {}", fjui_path);
        return;
    }

    stylesheet_ = {};
    if (!fjss_path.empty() && std::filesystem::exists(fjss_path)) {
        std::ifstream fjss_file(fjss_path);
        std::ostringstream fjss_ss;
        fjss_ss << fjss_file.rdbuf();
        stylesheet_ = FjssParser::parse(fjss_ss.str());
    }

    // Initialize text templates: save original text_content as template
    // before data binding resolves {key} placeholders
    init_text_templates(*root_);

    // Resolve src/material attributes to bindless indices
    if (resource_manager_) {
        resolve_textures(*root_);
    }

    dirty_ = true;

#ifndef NDEBUG
    loaded_fjui_path_ = fjui_path;
    loaded_fjss_path_ = fjss_path;
    watcher_ = std::make_unique<UiWatcher>(fjui_path, fjss_path,
        [this](const std::string& ui_path, const std::string& css_path) {
            // Reset watcher before reloading to avoid re-entrancy
            watcher_.reset();
            load(ui_path, css_path);
        });
#endif

    FJELL_CORE_INFO("GameUi: loaded {} ({} style rules)", fjui_path, stylesheet_.rules.size());
}

void GameUi::update(float viewport_width, float viewport_height,
                    uint32_t frame_index, float dt, const Input* input,
                    glm::vec2 viewport_offset) {
    FJELL_PROFILE_SCOPE_N("game_ui_update");
    if (!root_) return;

#ifndef NDEBUG
    if (watcher_) {
        (void)watcher_->poll();
    }
#endif

    needs_redraw_ = false;

    // 1. Data binding: resolve {key} templates and if directives
    bool bind_changed = data_bind_.resolve(*root_, context_);
    if (bind_changed) {
        dirty_ = true;
        needs_redraw_ = true;
    }

    // 2. Full style resolve (on first frame or when bindings changed)
    if (dirty_) {
        style_resolver_.resolve(*root_, stylesheet_);
        dirty_ = false;
        needs_redraw_ = true;
    }

    // 3. Process input: hit test, update hover/active/focus state
    if (input != nullptr) {
        bool input_changed = ui_input_.process(*root_, *input, context_,
                                                viewport_width, viewport_height,
                                                viewport_offset);
        // 4. Re-resolve only dirty nodes (state changed by input)
        if (input_changed) {
            needs_redraw_ = true;
            for (auto* node : ui_input_.dirty_nodes()) {
                style_resolver_.resolve_single(*node, stylesheet_);
            }
        }
    }

    // 5. Animate transitions
    bool animating = animation_.update(*root_, dt);
    if (animating) needs_redraw_ = true;

    // When something changes, flush all in-flight SSBO slots
    if (needs_redraw_) {
        redraw_countdown_ = MAX_FRAMES_IN_FLIGHT;
    }
    if (redraw_countdown_ == 0) return;
    --redraw_countdown_;

    // 6. Compute flexbox layout
    layout_.compute(*root_, viewport_width, viewport_height);

    // 7. Flatten quads to SSBO
    renderer_.update(*root_, viewport_width, viewport_height, frame_index);

    // 8. Shape and upload text for all text nodes
    if (font_atlas_.valid()) {
        shape_text_nodes(*root_, viewport_width, viewport_height, frame_index);
    }
}

void GameUi::shape_text_nodes(UiNode& node, float viewport_width, float viewport_height,
                               uint32_t frame_index) {
    if (node.hidden_by_if) return;

    if (!node.text_content.empty() && node.type == UiNodeType::text) {
        UiNode* layout_node = node.parent ? node.parent : &node;
        float max_width = layout_node->layout_rect.w;
        float font_size = layout_node->style.font_size;
        float line_height = layout_node->style.line_height;

        auto shaped = TextShaper::shape(node.text_content, font_atlas_,
                                        font_size, max_width, line_height);
        renderer_.update_text(shaped.glyphs, *layout_node,
                              font_atlas_.atlas_bindless_index(),
                              font_atlas_.pixel_range(),
                              viewport_width, viewport_height, frame_index);
    }

    for (auto& child : node.children) {
        shape_text_nodes(*child, viewport_width, viewport_height, frame_index);
    }
}

void GameUi::init_text_templates(UiNode& node) {
    if (node.type == UiNodeType::text && !node.text_content.empty()) {
        // If text contains {key} placeholders, save as template
        if (node.text_content.find('{') != std::string::npos) {
            node.text_template = node.text_content;
        }
    }
    for (auto& child : node.children) {
        init_text_templates(*child);
    }
}

void GameUi::record(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                    VkExtent2D extent, uint32_t frame_index) {
    FJELL_PROFILE_SCOPE_N("game_ui_record");
    renderer_.record(cmd, global_set, bindless_set, extent, frame_index);
}

void GameUi::resolve_textures(UiNode& node) {
    // Resolve src="path" → bindless texture index
    auto src_it = node.attributes.find("src");
    if (src_it != node.attributes.end() && !src_it->second.empty()) {
        auto resolved_path = base_path::resolve(src_it->second);
        auto tex_result = resource_manager_->load_texture(resolved_path);
        if (tex_result) {
            node.resolved_texture_id = resource_manager_->bindless_index(*tex_result);
        }
    }

    // Resolve material="name" → material SSBO index
    auto mat_it = node.attributes.find("material");
    if (mat_it != node.attributes.end() && !mat_it->second.empty()) {
        auto mat_handle = resource_manager_->find_material(mat_it->second);
        if (mat_handle.valid()) {
            auto* mat = resource_manager_->get(mat_handle);
            if (mat) {
                node.resolved_material_id = mat->material_id();
            }
        }
    }

    for (auto& child : node.children) {
        resolve_textures(*child);
    }
}

} // namespace fjell
