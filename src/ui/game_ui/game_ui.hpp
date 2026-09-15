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
#include <unordered_map>
#include <unordered_set>

namespace fjell {

class Device;
class BindlessTextures;
class FontLibrary;
class Input;
class ResourceManager;
struct PassResources;

/// Orchestrator for the game UI system.
/// Owns parsers, layout engine, style resolver, context and renderer; fonts
/// come from the renderer's FontLibrary, resolved per text node from its
/// `font-family` and `font-weight` through the sheet's @font-face rules.
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
                FontLibrary& fonts,
                VkFormat output_format = VK_FORMAT_UNDEFINED);

    void destroy();

    /// Load a .fjui + .fjss pair from disk.
    void load(const std::string& fjui_path, const std::string& fjss_path);

    /// Load from an already-parsed tree (used by .fjwidget inline markup).
    /// Takes ownership of the tree. fjss_path is resolved on disk.
    /// base_dir is used for widget tag resolution (widgets/<tag>.fjwidget).
    /// markup_path is the file the tree was parsed from, so an edit to it
    /// raises a reload request like an edit to any other source file.
    void load_from_tree(std::unique_ptr<UiNode> root, const std::string& fjss_path,
                        const std::string& base_dir, const std::string& markup_path);

    /// True once after any file this UI was built from — root markup,
    /// stylesheet, or a widget pulled in by tag — changed on disk. The owner
    /// rebuilds by loading again; until then the current tree stays intact,
    /// so nothing the owner still points into vanishes underneath it.
    /// Always false in release builds, which do not watch files.
    [[nodiscard]] bool consume_reload_request();

    /// Re-resolve styles, compute layout, shape text, process input.
    /// viewport_offset is the top-left of the viewport in window pixels (for input mapping).
    void update(float viewport_width, float viewport_height,
                uint32_t frame_index, float dt = 0.0f,
                const Input* input = nullptr,
                glm::vec2 viewport_offset = {0.0f, 0.0f});

    /// Record Vulkan draw commands.
    void record(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                VkExtent2D extent, uint32_t frame_index);

    /// Create worldspace pipelines (HDR + scene MSAA). Call after create().
    void create_world_pipelines(Device& device, const PassResources& resources);

    /// Record worldspace draw commands with push constants. prev_mvp is last
    /// frame's mvp for this same widget, for the velocity attachment.
    void record_world(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                      VkExtent2D extent, uint32_t frame_index, const UiWorldPushConstants& push,
                      const glm::mat4& prev_mvp);

    void set_resource_manager(ResourceManager* rm);
    void set_project(const class Project* project) { project_ = project; }

    /// Lay out and draw in this design frame, scaled to fit the real
    /// viewport (see UiDesignFrame). Percentages and viewport units measure
    /// the scaled frame, so they follow it.
    void set_design_frame(UiDesignFrame frame) { design_frame_ = frame; }

    /// Screen pixels per layout pixel as of the last update(): what the
    /// design frame scaled by. Anything that maps layout rects to the screen
    /// outside the renderer, such as editor overlays, multiplies by this.
    [[nodiscard]] float frame_scale() const { return frame_scale_; }
    /// The design-frame size the tree was last laid out in — what @media
    /// conditions and viewport units measure.
    [[nodiscard]] glm::vec2 layout_size() const { return {last_layout_width_, last_layout_height_}; }

    [[nodiscard]] UiContext& context() { return context_; }
    [[nodiscard]] const UiContext& context() const { return context_; }
    [[nodiscard]] UiNode* root() { return root_.get(); }
    [[nodiscard]] const UiNode* root() const { return root_.get(); }
    [[nodiscard]] Stylesheet& stylesheet() { return stylesheet_; }
    [[nodiscard]] const Stylesheet& stylesheet() const { return stylesheet_; }
    /// The root stylesheet's path as it was loaded — the `source` a rule
    /// made in the editor is given, so a save writes it to that file.
    [[nodiscard]] const std::string& stylesheet_path() const { return stylesheet_path_; }

    /// The owner is about to write this watched file itself. The change is
    /// swallowed instead of raising a reload request, since the tree in
    /// memory is exactly what is being written.
    void expect_write(const std::string& path);
    [[nodiscard]] bool loaded() const { return root_ != nullptr; }
    [[nodiscard]] bool needs_redraw() const;
    void mark_dirty();

private:
    void shape_text_nodes(UiNode& node, float viewport_width, float viewport_height,
                          uint32_t frame_index);
    /// The atlas a node's text draws in: the sheet's @font-face whose family
    /// matches and whose weight is nearest, else the default font. A family
    /// no face declares is reported once per load.
    [[nodiscard]] const FontAtlas& font_for(const UiComputedStyle& style);
    void init_text_templates(UiNode& node);
    void resolve_textures(UiNode& node);
    // Debug builds watch every file the loaded tree came from.
    void watch_sources(const std::string& markup_path, const std::string& fjss_path);
    // Every per-tree subsystem drops what it remembers about the old tree
    // before root_ is replaced.
    void discard_tree_state();

    std::unique_ptr<UiNode> root_;
    Stylesheet stylesheet_;
    std::string stylesheet_path_;
    StyleResolver style_resolver_;
    UiLayout layout_;
    UiContext context_;
    UiRenderer renderer_;
    UiInput ui_input_;
    UiDataBind data_bind_;
    UiAnimation animation_;
    WidgetResolver widget_resolver_;
    FontLibrary* fonts_{nullptr};
    // Resolved faces by "family\nweight", rebuilt as text is shaped after a
    // load; a sheet declares a handful of faces and text nodes ask every
    // frame, so the lookup is cached.
    std::unordered_map<std::string, const FontAtlas*> font_cache_;
    std::unordered_set<std::string> unknown_families_;
    bool dirty_{true};
    bool layout_dirty_{true};
    bool needs_redraw_{true};
    uint32_t redraw_countdown_{0}; // force N frames of redraw after a change

    // Last size update() computed layout at. A docked HUD's viewport can
    // resize (window resize, panel drag) without touching any binding,
    // style, input, or animation state that would otherwise set
    // layout_dirty_ — checked against every call so a size change alone
    // still triggers a relayout instead of stretching the stale one.
    float last_layout_width_{-1.0f};
    float last_layout_height_{-1.0f};
    UiDesignFrame design_frame_{};
    float frame_scale_{1.0f};

    ResourceManager* resource_manager_{nullptr};
    const class Project* project_{nullptr};
    BindlessTextures* bindless_{nullptr};

#ifndef NDEBUG
    std::unique_ptr<UiWatcher> watcher_;
    bool reload_requested_{false};
#endif
};

} // namespace fjell
