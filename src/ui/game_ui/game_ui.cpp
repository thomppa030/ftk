#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/fjui_parser.hpp"
#include "core/log.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fjell {

void GameUi::create(Device& device, const PassResources& resources,
                    VkFormat output_format) {
    renderer_.create(device, resources, output_format);
}

void GameUi::destroy() {
    renderer_.destroy();
}

void GameUi::load(const std::string& fjui_path, const std::string& fjss_path) {
    // Load .fjui markup
    if (!std::filesystem::exists(fjui_path)) {
        FJELL_CORE_WARN("GameUi: .fjui file not found: {}", fjui_path);
        return;
    }

    std::ifstream fjui_file(fjui_path);
    std::ostringstream fjui_ss;
    fjui_ss << fjui_file.rdbuf();
    std::string fjui_source = fjui_ss.str();

    root_ = FjuiParser::parse(fjui_source);
    if (!root_) {
        FJELL_CORE_ERROR("GameUi: failed to parse {}", fjui_path);
        return;
    }

    // Load .fjss stylesheet
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
        // Re-resolve styles from stylesheet
        style_resolver_.resolve(*root_, stylesheet_);
        dirty_ = false;
    }

    // Compute flexbox layout
    layout_.compute(*root_, viewport_width, viewport_height);

    // Flatten tree to GPU quads
    renderer_.update(*root_, viewport_width, viewport_height, frame_index);
}

void GameUi::record(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                    VkExtent2D extent, uint32_t frame_index) {
    renderer_.record(cmd, global_set, bindless_set, extent, frame_index);
}

} // namespace fjell
