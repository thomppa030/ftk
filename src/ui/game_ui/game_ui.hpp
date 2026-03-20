#pragma once

#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/font_atlas.hpp"
#include "ui/game_ui/text_shaper.hpp"
#include "ui/game_ui/ui_animation.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "ui/game_ui/ui_data_bind.hpp"
#include "ui/game_ui/ui_input.hpp"
#include "ui/game_ui/ui_layout.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "ui/game_ui/ui_renderer.hpp"
#include "ui/game_ui/style_resolver.hpp"

#ifndef NDEBUG
#include "ui/game_ui/ui_watcher.hpp"
#endif

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <memory>
#include <string>

namespace fjell {

class Device;
class BindlessTextures;
class Input;
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

    /// Re-resolve styles, compute layout, shape text, process input.
    /// viewport_offset is the top-left of the viewport in window pixels (for input mapping).
    void update(float viewport_width, float viewport_height,
                uint32_t frame_index, float dt = 0.0f,
                const Input* input = nullptr,
                glm::vec2 viewport_offset = {0.0f, 0.0f});

    /// Record Vulkan draw commands.
    void record(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                VkExtent2D extent, uint32_t frame_index);

    [[nodiscard]] UiContext& context() { return context_; }
    [[nodiscard]] bool loaded() const { return root_ != nullptr; }
    void mark_dirty() { dirty_ = true; }

private:
    void shape_text_nodes(UiNode& node, float viewport_width, float viewport_height,
                          uint32_t frame_index);
    void init_text_templates(UiNode& node);

    std::unique_ptr<UiNode> root_;
    Stylesheet stylesheet_;
    StyleResolver style_resolver_;
    UiLayout layout_;
    UiContext context_;
    UiRenderer renderer_;
    UiInput ui_input_;
    UiDataBind data_bind_;
    UiAnimation animation_;
    FontAtlas font_atlas_;
    bool dirty_{true};

#ifndef NDEBUG
    std::unique_ptr<UiWatcher> watcher_;
    std::string loaded_fjui_path_;
    std::string loaded_fjss_path_;
#endif
};

} // namespace fjell
