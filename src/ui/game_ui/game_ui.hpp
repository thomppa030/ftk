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
#include "ui/game_ui/widget_resolver.hpp"

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
class UploadContext;
class Input;
class ResourceManager;
struct PassResources;

/// Orchestrator for the game UI system.
/// Owns parsers, layout engine, style resolver, context, font atlas, and renderer.
///
/// Usage from game code:
///
///   game_ui.load("assets/ui/hud.fjui", "assets/ui/hud.fjss");
///
///   auto& ctx = game_ui.context();
///   ctx.set("player.health", "72");
///   ctx.set("player.max_health", "100");
///   ctx.set("show_debug", "1");
///   ctx.set("active_slot", "2");
///   ctx.bind_action("select_slot_1", [&ctx] { ctx.set("active_slot", "1"); });
///
class GameUi {
public:
    void create(Device& device, VmaAllocator allocator,
                const PassResources& resources,
                BindlessTextures& bindless,
                UploadContext& upload,
                const std::string& font_path,
                VkFormat output_format = VK_FORMAT_UNDEFINED);

    /// Create with a shared font atlas (avoids expensive re-generation).
    /// The caller must keep the source GameUi alive for the font atlas lifetime.
    void create_with_shared_font(Device& device, VmaAllocator allocator,
                                 const PassResources& resources,
                                 BindlessTextures& bindless,
                                 const FontAtlas& shared_atlas,
                                 VkFormat output_format = VK_FORMAT_UNDEFINED);

    void destroy();

    /// Load a .fjui + .fjss pair from disk.
    void load(const std::string& fjui_path, const std::string& fjss_path);

    /// Load from an already-parsed tree (used by .fjwidget inline markup).
    /// Takes ownership of the tree. fjss_path is resolved on disk.
    /// base_dir is used for widget tag resolution (widgets/<tag>.fjwidget).
    void load_from_tree(std::unique_ptr<UiNode> root, const std::string& fjss_path,
                        const std::string& base_dir);

    /// Re-resolve styles, compute layout, shape text, process input.
    /// viewport_offset is the top-left of the viewport in window pixels (for input mapping).
    void update(float viewport_width, float viewport_height,
                uint32_t frame_index, float dt = 0.0f,
                const Input* input = nullptr,
                glm::vec2 viewport_offset = {0.0f, 0.0f});

    /// Record Vulkan draw commands.
    void record(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                VkExtent2D extent, uint32_t frame_index);

    /// Create worldspace pipelines (HDR + scene MSAA). Call after create/create_with_shared_font.
    void create_world_pipelines(Device& device, const PassResources& resources);

    /// Record worldspace draw commands with push constants. prev_mvp is last
    /// frame's mvp for this same widget, for the velocity attachment.
    void record_world(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                      VkExtent2D extent, uint32_t frame_index, const UiWorldPushConstants& push,
                      const glm::mat4& prev_mvp);

    void set_resource_manager(ResourceManager* rm);
    void set_project(const class Project* project) { project_ = project; }

    [[nodiscard]] UiContext& context() { return context_; }
    [[nodiscard]] const UiContext& context() const { return context_; }
    [[nodiscard]] const FontAtlas& font() const { return shared_font_atlas_ ? *shared_font_atlas_ : font_atlas_; }
    [[nodiscard]] UiNode* root() { return root_.get(); }
    [[nodiscard]] const UiNode* root() const { return root_.get(); }
    [[nodiscard]] Stylesheet& stylesheet() { return stylesheet_; }
    [[nodiscard]] const Stylesheet& stylesheet() const { return stylesheet_; }
    [[nodiscard]] bool loaded() const { return root_ != nullptr; }
    [[nodiscard]] bool needs_redraw() const;
    void mark_dirty();

private:
    void shape_text_nodes(UiNode& node, float viewport_width, float viewport_height,
                          uint32_t frame_index);
    void init_text_templates(UiNode& node);
    void resolve_textures(UiNode& node);

    std::unique_ptr<UiNode> root_;
    Stylesheet stylesheet_;
    StyleResolver style_resolver_;
    UiLayout layout_;
    UiContext context_;
    UiRenderer renderer_;
    UiInput ui_input_;
    UiDataBind data_bind_;
    UiAnimation animation_;
    WidgetResolver widget_resolver_;
    FontAtlas font_atlas_;
    const FontAtlas* shared_font_atlas_{nullptr}; // non-owning, overrides font_atlas_ when set
    bool dirty_{true};
    bool layout_dirty_{true};
    bool needs_redraw_{true};
    uint32_t redraw_countdown_{0}; // force N frames of redraw after a change

    ResourceManager* resource_manager_{nullptr};
    const class Project* project_{nullptr};
    BindlessTextures* bindless_{nullptr};

#ifndef NDEBUG
    std::unique_ptr<UiWatcher> watcher_;
    std::string loaded_fjui_path_;
    std::string loaded_fjss_path_;
#endif
};

} // namespace fjell
