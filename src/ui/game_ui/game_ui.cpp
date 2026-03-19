#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/fjui_parser.hpp"
#include "renderer/device.hpp"
#include "core/log.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fjell {

void GameUi::create(Device& device, const PassResources& resources,
                    BindlessTextures& bindless,
                    VkCommandPool command_pool, VkQueue queue,
                    const std::string& font_path,
                    VkFormat output_format) {
    renderer_.create(device, resources, output_format);

    if (!font_path.empty() && std::filesystem::exists(font_path)) {
        font_atlas_.create(font_path, device.handle(), device.physical_device(),
                           command_pool, queue, bindless);
    }
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

    dirty_ = true;
    FJELL_CORE_INFO("GameUi: loaded {} ({} style rules)", fjui_path, stylesheet_.rules.size());
}

void GameUi::update(float viewport_width, float viewport_height, uint32_t frame_index) {
    if (!root_) return;

    if (dirty_) {
        style_resolver_.resolve(*root_, stylesheet_);
        dirty_ = false;
    }

    layout_.compute(*root_, viewport_width, viewport_height);
    renderer_.update(*root_, viewport_width, viewport_height, frame_index);

    // Shape and upload text for all text nodes
    if (font_atlas_.valid()) {
        shape_text_nodes(*root_, viewport_width, viewport_height, frame_index);
    }
}

void GameUi::shape_text_nodes(UiNode& node, float viewport_width, float viewport_height,
                               uint32_t frame_index) {
    if (!node.text_content.empty() && node.type == UiNodeType::text) {
        UiNode* layout_node = node.parent ? node.parent : &node;
        float max_width = layout_node->layout_rect.w;

        auto shaped = TextShaper::shape(node.text_content, font_atlas_,
                                        default_font_size_, max_width);
        renderer_.update_text(shaped.glyphs, *layout_node,
                              font_atlas_.atlas_bindless_index(),
                              font_atlas_.pixel_range(),
                              viewport_width, viewport_height, frame_index);
    }

    for (auto& child : node.children) {
        shape_text_nodes(*child, viewport_width, viewport_height, frame_index);
    }
}

void GameUi::record(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                    VkExtent2D extent, uint32_t frame_index) {
    renderer_.record(cmd, global_set, bindless_set, extent, frame_index);
}

} // namespace fjell
