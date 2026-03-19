#pragma once

#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/font_atlas.hpp"
#include "ui/game_ui/text_shaper.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "ui/game_ui/ui_layout.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "ui/game_ui/ui_renderer.hpp"
#include "ui/game_ui/style_resolver.hpp"

#include <vulkan/vulkan.h>

#include <memory>
#include <string>

namespace fjell {

class Device;
class BindlessTextures;
struct PassResources;

/// Orchestrator for the game UI system.
/// Owns parsers, layout engine, style resolver, context, font atlas, and renderer.
class GameUi {
public:
    void create(Device& device, const PassResources& resources,
                BindlessTextures& bindless,
                VkCommandPool command_pool, VkQueue queue,
                const std::string& font_path,
                VkFormat output_format = VK_FORMAT_UNDEFINED);
    void destroy();

    /// Load a .fjui + .fjss pair from disk.
    void load(const std::string& fjui_path, const std::string& fjss_path);

    /// Re-resolve styles, compute layout, shape text.
    void update(float viewport_width, float viewport_height, uint32_t frame_index);

    /// Record Vulkan draw commands.
    void record(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                VkExtent2D extent, uint32_t frame_index);

    [[nodiscard]] UiContext& context() { return context_; }
    [[nodiscard]] bool loaded() const { return root_ != nullptr; }

private:
    void shape_text_nodes(UiNode& node, float viewport_width, float viewport_height,
                          uint32_t frame_index);

    std::unique_ptr<UiNode> root_;
    Stylesheet stylesheet_;
    StyleResolver style_resolver_;
    UiLayout layout_;
    UiContext context_;
    UiRenderer renderer_;
    FontAtlas font_atlas_;
    float default_font_size_{16.0f};
    bool dirty_{true};
};

} // namespace fjell
