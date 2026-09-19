#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/fjss_loader.hpp"
#include "ui/game_ui/fjui_parser.hpp"
#include "ui/game_ui/font_library.hpp"
#include "ui/game_ui/ui_data_bind.hpp"
#include "renderer/bindless.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "core/base_path.hpp"
#include "core/input.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/project.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fjell {

void GameUi::create(Device& device, VmaAllocator allocator,
                    const PassResources& resources,
                    BindlessTextures& bindless,
                    FontLibrary& fonts,
                    PipelineCache* fjsl_pipelines,
                    DeferredDeleter& deleter,
                    VkFormat output_format) {
    bindless_ = &bindless;
    fonts_ = &fonts;
    renderer_.create(device, allocator, resources, fjsl_pipelines, deleter, output_format);
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

    auto parsed = FjuiParser::parse(fjui_ss.str());
    if (!parsed) {
        FJELL_CORE_ERROR("GameUi: failed to parse {}", fjui_path);
        return;
    }
    ui_stamp_source_file(*parsed, fjui_path);
    discard_tree_state();
    root_ = std::move(parsed);

    stylesheet_ = {};
    stylesheet_path_ = fjss_path;
    if (!fjss_path.empty() && std::filesystem::exists(fjss_path)) {
        stylesheet_ = load_stylesheet(fjss_path);
    }

    // Resolve widget tags: <health-bar/> → widgets/health_bar.fjui
    auto base_dir = std::filesystem::path(fjui_path).parent_path().string();
    widget_resolver_.resolve(*root_, base_dir, stylesheet_, context_);

    // Initialize text templates: save original text_content as template
    // before data binding resolves {key} placeholders
    init_text_templates(*root_);

    // Resolve src/material attributes to bindless indices
    if (resource_manager_) {
        resolve_textures(*root_);
    }

    dirty_ = true;
    watch_sources(fjui_path, fjss_path);

    FJELL_CORE_DEBUG("GameUi: loaded {} ({} style rules)", fjui_path, stylesheet_.rules.size());
}

void GameUi::load_from_tree(std::unique_ptr<UiNode> root, const std::string& fjss_path,
                             const std::string& base_dir, const std::string& markup_path) {
    if (!root) return;
    discard_tree_state();
    stylesheet_ = {};
    stylesheet_path_ = fjss_path;
    if (!fjss_path.empty() && std::filesystem::exists(fjss_path)) {
        stylesheet_ = load_stylesheet(fjss_path);
    }
    install_tree(std::move(root), base_dir);
    watch_sources(markup_path, fjss_path);
}

void GameUi::load_from_tree(std::unique_ptr<UiNode> root, Stylesheet sheet,
                             const std::string& base_dir) {
    if (!root) return;
    discard_tree_state();
    stylesheet_ = std::move(sheet);
    stylesheet_path_.clear();
    install_tree(std::move(root), base_dir);
    watch_sources({}, {});
}

void GameUi::install_tree(std::unique_ptr<UiNode> root, const std::string& base_dir) {
    root_ = std::move(root);
    widget_resolver_.resolve(*root_, base_dir, stylesheet_, context_);
    init_text_templates(*root_);

    // Run an initial data_bind pass to sync prop_map values into scoped
    // contexts before resolve_textures (material attributes may reference
    // {prop} bindings) and before any controller's begin_play reads them.
    data_bind_.resolve(*root_, context_);

    if (resource_manager_) {
        resolve_textures(*root_);
    }

    dirty_ = true;
    FJELL_CORE_DEBUG("GameUi: loaded from tree ({} style rules)", stylesheet_.rules.size());
}

void GameUi::discard_tree_state() {
    data_bind_.forget_tree();
    ui_input_.forget_tree();
    animation_.forget_tree();
    bar_dynamics_.forget_tree();
    // The faces come with the sheet, which the load replaces.
    font_cache_.clear();
    unknown_families_.clear();
}

const FontAtlas& GameUi::font_for(const UiComputedStyle& style) {
    const auto& fallback = fonts_->fallback();
    if (style.font_family.empty()) return fallback;

    char weight_text[16];
    std::snprintf(weight_text, sizeof(weight_text), "%g", static_cast<double>(style.font_weight));
    std::string key = style.font_family + '\n' + weight_text;
    if (auto it = font_cache_.find(key); it != font_cache_.end()) return *it->second;

    // The face of the family whose weight is nearest; a tie goes to the
    // lighter one, the way CSS resolves a weight below 500.
    const StyleFontFace* best = nullptr;
    for (const auto& face : stylesheet_.font_faces) {
        if (face.family != style.font_family) continue;
        if (!best) {
            best = &face;
            continue;
        }
        float d_best = std::abs(best->weight - style.font_weight);
        float d_face = std::abs(face.weight - style.font_weight);
        if (d_face < d_best || (d_face == d_best && face.weight < best->weight)) best = &face;
    }

    if (!best) {
        if (unknown_families_.insert(style.font_family).second) {
            FJELL_CORE_WARN("GameUi: {}: no @font-face declares font-family '{}', using the default font",
                            stylesheet_path_.empty() ? "<in-memory sheet>" : stylesheet_path_,
                            style.font_family);
        }
        font_cache_.emplace(std::move(key), &fallback);
        return fallback;
    }

    // Nullptr while the library is still loading the face (or never will):
    // draw in the fallback for now and ask again next frame, so the text
    // switches the frame the atlas arrives.
    const FontAtlas* atlas = fonts_->get(best->resolved_src);
    if (!atlas) return fallback;
    font_cache_.emplace(std::move(key), atlas);
    return *atlas;
}

void GameUi::watch_sources(const std::string& markup_path, const std::string& fjss_path) {
#ifndef NDEBUG
    std::vector<std::string> paths{markup_path, fjss_path};
    // Imported sheets and widget files: every file the tree was built from.
    for (const auto& file : stylesheet_.files) {
        if (std::find(paths.begin(), paths.end(), file) == paths.end()) paths.push_back(file);
    }
    const auto& widget_files = widget_resolver_.dependencies();
    paths.insert(paths.end(), widget_files.begin(), widget_files.end());
    watcher_ = std::make_unique<UiWatcher>(paths, [this] { reload_requested_ = true; });
#else
    (void)markup_path;
    (void)fjss_path;
#endif
}

void GameUi::expect_write(const std::string& path) {
#ifndef NDEBUG
    if (watcher_) watcher_->expect_change(path);
#else
    (void)path;
#endif
}

bool GameUi::consume_reload_request() {
#ifndef NDEBUG
    bool requested = reload_requested_;
    reload_requested_ = false;
    return requested;
#else
    return false;
#endif
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

    // Design-frame scaling: everything below works in layout pixels of the
    // design frame, scaled so the frame fits the real viewport. The
    // renderer maps layout pixels onto the target by this one factor, the
    // mouse is brought into layout space by it, and layout rounds to it so
    // edges land on real pixels.
    float scale = design_frame_.scale_for(viewport_width, viewport_height);
    frame_scale_ = scale;
    viewport_width /= scale;
    viewport_height /= scale;

    // The viewport can resize (window resize, docked panel drag) without any
    // binding, style, input, or animation change firing below — check for it
    // directly so the layout doesn't keep stretching a stale computation at
    // the old size.
    if (viewport_width != last_layout_width_ || viewport_height != last_layout_height_) {
        last_layout_width_ = viewport_width;
        last_layout_height_ = viewport_height;
        // Viewport-relative font sizes are resolved in the cascade, so a
        // resize is a style change as well as a layout change.
        style_resolver_.set_viewport(viewport_width, viewport_height);
        dirty_ = true;
        layout_dirty_ = true;
        needs_redraw_ = true;
    }

    // 1. Data binding: resolve {key} templates and if directives
    auto bind_result = data_bind_.resolve(*root_, context_);
    if (bind_result.layout_changed) {
        dirty_ = true;
        layout_dirty_ = true;
        needs_redraw_ = true;
    } else if (bind_result.text_changed) {
        needs_redraw_ = true;
    }

    // 2. Full style resolve (on first frame or when layout-affecting bindings changed)
    if (dirty_) {
        style_resolver_.resolve(*root_, stylesheet_);
        dirty_ = false;
        layout_dirty_ = true;
        needs_redraw_ = true;
    }

    // 3. Process input: hit test, update hover/active/focus state
    if (input != nullptr) {
        bool input_changed = ui_input_.process(*root_, *input, context_,
                                                viewport_width, viewport_height,
                                                viewport_offset, scale);
        // 4. Re-resolve only dirty nodes (state changed by input)
        if (input_changed) {
            needs_redraw_ = true;
            layout_dirty_ = true;
            for (auto* node : ui_input_.dirty_nodes()) {
                style_resolver_.resolve_subtree(*node, stylesheet_);
            }
        }
    }

    // 5. Move every bar's displayed fill toward its value. Only the quads
    // change, so a redraw is enough.
    if (bar_dynamics_.update(*root_, dt)) {
        needs_redraw_ = true;
    }

    // 6. Animate transitions
    bool animating = animation_.update(*root_, dt);
    if (animating) {
        needs_redraw_ = true;
        layout_dirty_ = true;
    }

    // When something changes, flush all in-flight SSBO slots
    if (needs_redraw_) {
        redraw_countdown_ = MAX_FRAMES_IN_FLIGHT;
    }
    if (redraw_countdown_ == 0) return;
    --redraw_countdown_;

    // 6. Compute flexbox layout (only when structure/style changed, not text-only)
    if (dirty_ || layout_dirty_) {
        layout_.compute(*root_, viewport_width, viewport_height, scale);
        layout_dirty_ = false;
    }

    // 7. Flatten quads to SSBO
    renderer_.update(*root_, viewport_width, viewport_height, scale, frame_index);

    // 8. Shape and upload text for all text nodes
    if (fonts_ && fonts_->fallback().valid()) {
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

        const auto& fa = font_for(layout_node->style);
        auto shaped = TextShaper::shape(node.text_content, fa,
                                        font_size, max_width, line_height);
        renderer_.update_text(shaped.glyphs, *layout_node,
                              fa.atlas_bindless_index(),
                              fa.pixel_range(),
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

void GameUi::create_world_pipelines(Device& device, const PassResources& resources) {
    renderer_.create_world_pipelines(device, resources);
}

void GameUi::record_world(VkCommandBuffer cmd, VkDescriptorSet global_set, VkDescriptorSet bindless_set,
                           VkExtent2D extent, uint32_t frame_index, const UiWorldPushConstants& push,
                           const glm::mat4& prev_mvp) {
    renderer_.record_world(cmd, global_set, bindless_set, extent, frame_index, push, prev_mvp);
}

void GameUi::resolve_textures(UiNode& node) {
    // An attribute may name its asset through a {key} template. Inside a
    // widget instance the nearest scoped context holds its props; a tree
    // loaded on its own (editor preview, thumbnail) has none and reads the
    // root context, where the loader seeded the prop defaults.
    auto resolve_reference = [&](const std::string& value) {
        if (value.find('{') == std::string::npos) return value;
        UiContext* ctx = &context_;
        for (auto* n = &node; n; n = n->parent) {
            if (n->scoped_context) { ctx = n->scoped_context.get(); break; }
        }
        return UiDataBind::resolve_template(value, *ctx);
    };

    // Resolve src="path" → bindless texture index. A project's own textures
    // resolve against the project, like materials do; without a project the
    // path is engine-relative.
    auto src_it = node.attributes.find("src");
    if (src_it != node.attributes.end() && !src_it->second.empty()) {
        std::string src = resolve_reference(src_it->second);
        auto resolved_path = src.empty() ? std::string{} : base_path::resolve_asset(src, project_);
        auto tex_result = resolved_path.empty()
            ? Result<TextureHandle>(make_error("asset reference does not resolve"))
            : resource_manager_->load_texture(resolved_path);
        if (tex_result) {
            node.resolved_texture_id = resource_manager_->bindless_index(*tex_result);
        } else {
            FJELL_CORE_WARN("{}:{}: <{}> src='{}' did not load: {}",
                            node.source_file, node.source_line, node.tag, src_it->second,
                            tex_result.error());
        }
    }

    // Resolve material="name" → material SSBO index
    // Try by name first (engine-created materials like "default"),
    // then as a .fjmat asset path.
    auto mat_it = node.attributes.find("material");
    if (mat_it != node.attributes.end() && !mat_it->second.empty()) {
        std::string mat_name = resolve_reference(mat_it->second);
        auto mat_handle = resource_manager_->find_material(mat_name);
        if (!mat_handle.valid()) {
            // Try loading as .fjmat asset — resolve via project if available
            std::string mat_path = mat_name;
            if (!mat_path.ends_with(".fjmat")) {
                mat_path = "assets/materials/" + mat_path + ".fjmat";
            }
            if (project_) {
                mat_path = project_->resolve(mat_path);
            } else {
                mat_path = base_path::resolve(mat_path);
            }
            if (std::filesystem::exists(mat_path)) {
                auto result = resource_manager_->load_material_asset(mat_path, project_);
                if (result) mat_handle = *result;
            }
        }
        if (mat_handle.valid()) {
            auto* mat = resource_manager_->get(mat_handle);
            if (mat && mat->shader_type() != ShaderType::Ui) {
                // A UI quad is drawn by the material's own fragment pipeline,
                // which only a `shader_type ui;` shader has. Reported where
                // the stylesheet problems are, so the editor lists it.
                stylesheet_.diagnostics.push_back({
                    node.source_file, node.source_line, 1, FjssSeverity::error,
                    "material '" + mat_name + "' is shader_type "
                        + std::string(shader_type_name(mat->shader_type())) +
                        "; a UI node needs a shader_type ui material"});
                FJELL_CORE_WARN("{}:{}: <{}> material '{}' is shader_type {}, a UI node needs "
                                "shader_type ui",
                                node.source_file, node.source_line, node.tag, mat_name,
                                shader_type_name(mat->shader_type()));
            } else if (mat) {
                node.resolved_material_id = mat->material_id();
                node.resolved_pipeline_id = mat->pipeline_id();
                FJELL_CORE_DEBUG("GameUi: <{}> material '{}' -> material_id={} pipeline_id={} "
                                 "param_offset={} param_count={}",
                                 node.tag, mat_name, mat->material_id(), mat->pipeline_id(),
                                 mat->custom_param_offset(), mat->custom_param_count());
            }
        } else {
            FJELL_CORE_WARN("GameUi: <{}> material='{}' did not resolve to a valid handle",
                            node.tag, mat_name);
        }
    }

    for (auto& child : node.children) {
        resolve_textures(*child);
    }
}

} // namespace fjell
