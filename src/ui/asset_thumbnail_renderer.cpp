#include "ui/asset_thumbnail_renderer.hpp"

#include "environment/environment_resolver.hpp"
#include "ui/game_ui/font_sample.hpp"
#include "ui/game_ui/font_library.hpp"
#include "ui/game_ui/fjfont.hpp"

#include "animation/animation_clip.hpp"
#include "animation/animator_component.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project.hpp"
#include "renderer/camera.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/particle_emitter.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/gpu/image_readback.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/post_pass.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resources/fjmesh_writer.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/material_asset.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/systems/vfx_asset.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/viewport_source.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/light_node.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/game_ui/ui_data_bind.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "ui/game_ui/widget_asset.hpp"
#include "ui/icon_cache.hpp"

#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

namespace fjell {

namespace fs = std::filesystem;

namespace {

constexpr const char* GRAPH_PATH = "assets/render_graphs/asset_thumbnail.fjgraph";
// A saved scene is shot with shadows and its own sky, the way the editor
// shows it; the camera preview renders through this graph for the same
// reason.
constexpr const char* SCENE_GRAPH_PATH = "assets/render_graphs/editor_preview.fjgraph";

// The camera preview waits this long too: MAX_FRAMES_IN_FLIGHT command
// buffers may still be pending when the private set is allocated.
constexpr int DRAIN_FRAMES = 3;

// Cache hits are a PNG decode and a small upload each, so several fit in a
// frame; a render is a full pipeline submit and gets the frame to itself.
constexpr int CACHE_HITS_PER_TICK = 8;

// Three-quarter view with the subject's bounding sphere filling most of
// the frame. The floor keeps a tiny mesh in front of the near plane.
constexpr float CAMERA_DISTANCE_PER_RADIUS = 3.1F;
constexpr float CAMERA_MIN_DISTANCE = 0.3F;
constexpr float CAMERA_YAW = -30.0F;
constexpr float CAMERA_PITCH = 20.0F;

fs::file_time_type file_time(const fs::path& path) {
    std::error_code ec;
    auto t = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type::min() : t;
}

// The material every scene object falls back to, so an unassigned slot
// looks the same in the thumbnail as it does in the viewport.
MaterialHandle default_material(ResourceManager& resources) {
    auto tex = resources.load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    return resources.create_material("default", tex);
}

AssetThumbnailRenderer::Framing framing_of(const AABB& aabb, float bounding_radius) {
    AssetThumbnailRenderer::Framing framing;
    if (aabb.valid()) framing.center = (aabb.min + aabb.max) * 0.5F;
    framing.radius = std::max(bounding_radius, 1e-3F);
    return framing;
}

// ── Subjects ─────────────────────────────────────────────────────────────

class MaterialSubject final : public AssetThumbnailRenderer::SceneSubject {
public:
    [[nodiscard]] bool handles(const fs::path& path) const override {
        return path.extension() == ".fjmat";
    }

    [[nodiscard]] fs::file_time_type source_time(const fs::path& path,
                                                 const Project* project) const override {
        auto newest = file_time(path);
        if (newest == fs::file_time_type::min()) return newest;

        // The look also depends on the shader and the textures, so an edit
        // to any of them is a newer source. A reference that does not
        // resolve is ignored: a broken material still gets one thumbnail,
        // not one per frame.
        MaterialAsset asset;
        try {
            asset = load_material_asset(path.string());
        } catch (const std::exception&) {
            return newest;
        }
        auto consider = [&](const std::string& reference) {
            if (reference.empty() || reference.starts_with("$viewport:")) return;
            auto resolved = base_path::resolve_asset(reference, project);
            if (!resolved.empty()) newest = std::max(newest, file_time(resolved));
        };
        consider(asset.shader);
        for (const auto& [slot, texture] : asset.textures) consider(texture);
        return newest;
    }

    bool populate(Scene& scene, const AssetThumbnailRenderer::Deps& deps,
                  const std::string& path, AssetThumbnailRenderer::Framing& framing) override {
        auto& resources = *deps.resource_manager;
        auto sphere = resources.create_primitive("sphere");
        if (!sphere) {
            FJELL_CORE_ERROR("Material thumbnail: {}", sphere.error());
            return false;
        }
        auto material = resources.load_material_asset(path, deps.project);
        if (!material) {
            FJELL_CORE_WARN("Material thumbnail: {}", material.error());
            return false;
        }
        // A ui material draws quads and a terrain material draws a
        // heightfield, not a sphere; both keep the generic icon.
        if (const auto* loaded = resources.get(*material);
            loaded && loaded->shader_type() != ShaderType::Spatial) {
            FJELL_CORE_DEBUG("Material thumbnail: '{}' is a {} shader, no sphere preview",
                             path, shader_type_name(loaded->shader_type()));
            return false;
        }
        scene.add(*sphere, *material, resources, {}, "Thumbnail Sphere", "primitive:sphere", path);
        // The primitive is a unit-diameter sphere at the origin.
        framing = {.center = {0.0F, 0.0F, 0.0F}, .radius = 0.5F};
        return true;
    }
};

class MeshSubject final : public AssetThumbnailRenderer::SceneSubject {
public:
    [[nodiscard]] bool handles(const fs::path& path) const override {
        auto ext = path.extension();
        return ext == ".fjmesh" || ext == ".glb" || ext == ".gltf";
    }

    [[nodiscard]] fs::file_time_type source_time(const fs::path& path,
                                                 const Project* project) const override {
        auto newest = file_time(path);
        if (newest == fs::file_time_type::min() || path.extension() != ".fjmesh") return newest;

        // The slot materials are part of the picture, and each of them has
        // its own references. The authored block reads without the geometry.
        MaterialSubject material;
        for (const auto& slot : read_authored_data(path.string()).material_slots) {
            if (slot.path.empty()) continue;
            auto resolved = base_path::resolve_asset(slot.path, project);
            if (!resolved.empty()) newest = std::max(newest, material.source_time(resolved, project));
        }
        return newest;
    }

    bool populate(Scene& scene, const AssetThumbnailRenderer::Deps& deps,
                  const std::string& path, AssetThumbnailRenderer::Framing& framing) override {
        auto& resources = *deps.resource_manager;
        auto model = resources.load_model(path);
        if (!model) {
            FJELL_CORE_WARN("Mesh thumbnail: {}", model.error());
            return false;
        }
        const auto* loaded = resources.get(*model);
        if (!loaded) return false;
        // A skinned .fjmesh loads as an empty model; its picture is the
        // .fjskel's, not a frame of sky.
        if (loaded->mesh_count() == 0) return false;

        uint32_t id = scene.add(*model, default_material(resources), resources, {},
                                fs::path(path).stem().string(), path, "default");
        if (auto* node = scene.find(id)) {
            if (auto* mesh = node->components.get<MeshComponent>()) {
                mesh->slot_materials = resources.load_slot_materials(*model, deps.project);
            }
        }
        framing = framing_of(loaded->local_aabb(), loaded->bounding_radius());
        return true;
    }
};

// Adds a .fjskel to the scene with the default material and no animator,
// so the bone upload hands it its bind pose. Returns the node, or null when
// the file did not load.
Node3D* add_skeletal_mesh(Scene& scene, const AssetThumbnailRenderer::Deps& deps,
                          const std::string& skel_path, AssetThumbnailRenderer::Framing& framing) {
    auto& resources = *deps.resource_manager;
    auto model = resources.load_skeletal_model(skel_path);
    if (!model) {
        FJELL_CORE_WARN("Skeletal mesh thumbnail: {}", model.error());
        return nullptr;
    }
    auto* loaded = resources.get(*model);
    if (!loaded) return nullptr;

    auto node = std::make_unique<Node3D>();
    node->name = fs::path(skel_path).stem().string();
    node->source = skel_path;
    auto& skinned = node->components.add<SkeletalMeshComponent>();
    skinned.skeletal_model_handle = *model;
    skinned.skeletal_model = loaded;
    skinned.material_handle = default_material(resources);
    skinned.material = resources.get(skinned.material_handle);
    skinned.material_name = "default";
    auto* added = node.get();
    scene.add_node(std::move(node));

    framing = framing_of(loaded->local_aabb(), loaded->bounding_radius());
    return added;
}

class SkeletalMeshSubject final : public AssetThumbnailRenderer::SceneSubject {
public:
    [[nodiscard]] bool handles(const fs::path& path) const override {
        return path.extension() == ".fjskel";
    }

    [[nodiscard]] fs::file_time_type source_time(const fs::path& path,
                                                 const Project* /*project*/) const override {
        return file_time(path);
    }

    bool populate(Scene& scene, const AssetThumbnailRenderer::Deps& deps,
                  const std::string& path, AssetThumbnailRenderer::Framing& framing) override {
        return add_skeletal_mesh(scene, deps, path, framing) != nullptr;
    }
};

// A clip carries no skeleton reference; the animation editor pairs clips
// with the .fjskel they sit beside, and the thumbnail follows the same
// convention. Zero or several skeletons in the directory means no guess.
fs::path skeleton_beside(const fs::path& clip) {
    fs::path found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(clip.parent_path(), ec)) {
        if (entry.path().extension() != ".fjskel") continue;
        if (!found.empty()) return {};
        found = entry.path();
    }
    return found;
}

class AnimationClipSubject final : public AssetThumbnailRenderer::SceneSubject {
public:
    [[nodiscard]] bool handles(const fs::path& path) const override {
        return path.extension() == ".fjanim";
    }

    [[nodiscard]] fs::file_time_type source_time(const fs::path& path,
                                                 const Project* /*project*/) const override {
        auto newest = file_time(path);
        auto skeleton = skeleton_beside(path);
        if (!skeleton.empty()) newest = std::max(newest, file_time(skeleton));
        return newest;
    }

    bool populate(Scene& scene, const AssetThumbnailRenderer::Deps& deps,
                  const std::string& path, AssetThumbnailRenderer::Framing& framing) override {
        auto skeleton = skeleton_beside(path);
        if (skeleton.empty()) {
            FJELL_CORE_WARN("Animation thumbnail: no single .fjskel beside {}", path);
            return false;
        }
        auto clip = AnimationClip::load(path);
        if (!clip) {
            FJELL_CORE_WARN("Animation thumbnail: {}", clip.error());
            return false;
        }
        // The animator keeps a pointer to the clip, so it lives here until
        // the next clip replaces it, past the render that samples it.
        clip_ = std::move(*clip);

        auto* node = add_skeletal_mesh(scene, deps, skeleton.string(), framing);
        if (!node) return false;
        auto* skinned = node->components.get<SkeletalMeshComponent>();
        if (!skinned || !skinned->skeletal_model->skeleton()) {
            FJELL_CORE_WARN("Animation thumbnail: {} has no embedded skeleton", skeleton.string());
            return false;
        }

        // A frozen frame from the middle of the clip: enough to tell a jump
        // from an idle, and the bone upload takes the pose from here.
        auto& animator = node->components.add<AnimatorComponent>();
        animator.init(skinned->skeletal_model->skeleton());
        animator.play(&*clip_, false);
        animator.set_time(clip_->duration() * 0.5F);
        animator.update(0.0F);
        return true;
    }

private:
    std::optional<AnimationClip> clip_;
};

// ── Effects ──────────────────────────────────────────────────────────────

// A particle effect is nothing at frame zero, so its thumbnail comes from
// a simulation stepped up to a moment where the effect has filled in, in
// one go, then rendered once. The particle system keeps emitters per
// scene, so the steps leave every other scene's effects untouched.
class VfxSubject final : public AssetThumbnailRenderer::Subject {
public:
    [[nodiscard]] bool handles(const fs::path& path) const override {
        return path.extension() == ".fjvfx";
    }

    [[nodiscard]] fs::file_time_type source_time(const fs::path& path,
                                                 const Project* /*project*/) const override {
        return file_time(path);
    }

    bool render(const AssetThumbnailRenderer::Deps& deps, AssetThumbnailRenderer::SceneStage& stage,
                float /*dt*/, const std::string& path,
                AssetThumbnailRenderer::Pixels& out) override {
        auto& vk = *deps.vulkan_context;
        if (!stage.ensure(deps)) return false;
        const auto* graph = vk.get_or_load_graph(VFX_GRAPH_PATH);
        if (graph == nullptr) {
            FJELL_CORE_ERROR("Effect thumbnails disabled: failed to load {}", VFX_GRAPH_PATH);
            return false;
        }

        // The effect's timing state lives on the VfxEffect itself, so the
        // thumbnail runs its own copy rather than the one a scene may be
        // playing.
        std::shared_ptr<VfxEffect> effect;
        try {
            auto loaded = load_vfx_effect(path);
            resolve_vfx_textures(loaded, *deps.resource_manager, deps.project);
            effect = std::make_shared<VfxEffect>(std::move(loaded));
        } catch (const std::exception& e) {
            FJELL_CORE_WARN("Effect thumbnail: {}: {}", path, e.what());
            return false;
        }
        if (effect->emitters.empty()) return false;

        auto& scene = stage.begin();
        auto node = std::make_unique<Node3D>();
        node->name = fs::path(path).stem().string();
        auto& emitter = node->components.add<EmitterComponent>();
        emitter.effect = effect;
        emitter.vfx_path = path;
        scene.add_node(std::move(node));
        scene.compute_world_matrices(nullptr);
        vk.sync_scene_particles(scene);

        // Long enough for the first particles to reach the end of their
        // life, so a looping effect is at its steady state and a burst is
        // mid-flight; capped so a slow effect does not stall the editor.
        float warm = std::clamp(effect->max_lifetime(), MIN_WARMUP_SECONDS, MAX_WARMUP_SECONDS);
        int steps = static_cast<int>(std::ceil(warm / WARMUP_STEP));
        for (int i = 0; i < steps; ++i) {
            if (!stage.render(deps, WARMUP_STEP, FRAMING, nullptr, graph)) return false;
        }
        // The simulation advances the effect's own clock, so this says
        // whether the steps reached the emitter at all.
        for (size_t i = 0; i < effect->emitters.size(); ++i) {
            const auto& sub = effect->emitters[i];
            FJELL_CORE_DEBUG("Effect thumbnail {} emitter {}: {} steps, elapsed {:.2f}s, "
                             "rate {:.1f}/s, burst {}, lifetime {:.2f}s, paused {}, finished {}",
                             fs::path(path).filename().string(), i, steps, sub.emitter_elapsed,
                             sub.emission_rate(), sub.burst_count(), sub.lifetime_max(),
                             sub.paused, sub.finished);
        }
        return stage.render(deps, WARMUP_STEP, FRAMING, &out, graph);
    }

private:
    static constexpr const char* VFX_GRAPH_PATH = "assets/render_graphs/asset_thumbnail_vfx.fjgraph";
    static constexpr float WARMUP_STEP = 1.0F / 30.0F;
    static constexpr float MIN_WARMUP_SECONDS = 0.5F;
    static constexpr float MAX_WARMUP_SECONDS = 2.0F;
    // The VFX editor frames its preview the same way.
    static constexpr AssetThumbnailRenderer::Framing FRAMING{.center = {0.0F, 1.0F, 0.0F}, .radius = 3.0F};
};

// ── Scenes ───────────────────────────────────────────────────────────────

// A scene's thumbnail is taken when the scene is saved, from the scene
// already in memory (capture_scene). Rendering one on demand would mean
// loading every asset in it, so this subject only ever serves the cache.
class SceneFileSubject final : public AssetThumbnailRenderer::Subject {
public:
    [[nodiscard]] bool handles(const fs::path& path) const override {
        return path.extension() == ".fjell";
    }

    [[nodiscard]] fs::file_time_type source_time(const fs::path& path,
                                                 const Project* /*project*/) const override {
        return file_time(path);
    }

    bool render(const AssetThumbnailRenderer::Deps& /*deps*/, AssetThumbnailRenderer::SceneStage& /*stage*/,
                float /*dt*/, const std::string& /*path*/,
                AssetThumbnailRenderer::Pixels& /*out*/) override {
        return false;
    }

    [[nodiscard]] bool renders_on_demand() const override { return false; }
};

// ── UI ───────────────────────────────────────────────────────────────────

// The stylesheet a markup file draws with: the one its <widget> header names,
// else the sheet of the same name beside it.
fs::path stylesheet_for(const fs::path& markup) {
    if (markup.extension() == ".fjwidget") {
        auto asset = load_widget_asset(markup.string());
        if (asset && !asset->fjss_path.empty()) return markup.parent_path() / asset->fjss_path;
    }
    fs::path fjss = markup;
    fjss.replace_extension(".fjss");
    return fjss;
}

// The design-frame pixels the tree's content covers: the union of every
// node below the root. The root itself is laid out to the frame, so it
// says nothing about the widget's own size.
VkRect2D content_bounds(const UiNode& root, uint32_t frame_width, uint32_t frame_height) {
    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();
    std::vector<const UiNode*> stack;
    for (const auto& child : root.children) stack.push_back(child.get());
    while (!stack.empty()) {
        const UiNode* node = stack.back();
        stack.pop_back();
        const auto& r = node->layout_rect;
        if (r.w > 0.0F && r.h > 0.0F) {
            min_x = std::min(min_x, r.x);
            min_y = std::min(min_y, r.y);
            max_x = std::max(max_x, r.x + r.w);
            max_y = std::max(max_y, r.y + r.h);
        }
        for (const auto& child : node->children) stack.push_back(child.get());
    }

    auto fw = static_cast<float>(frame_width);
    auto fh = static_cast<float>(frame_height);
    if (min_x >= max_x || min_y >= max_y) return {{0, 0}, {frame_width, frame_height}};
    min_x = std::clamp(std::floor(min_x), 0.0F, fw);
    min_y = std::clamp(std::floor(min_y), 0.0F, fh);
    max_x = std::clamp(std::ceil(max_x), 0.0F, fw);
    max_y = std::clamp(std::ceil(max_y), 0.0F, fh);
    if (max_x - min_x < 1.0F || max_y - min_y < 1.0F) return {{0, 0}, {frame_width, frame_height}};
    return {{static_cast<int32_t>(min_x), static_cast<int32_t>(min_y)},
            {static_cast<uint32_t>(max_x - min_x), static_cast<uint32_t>(max_y - min_y)}};
}

// What every subject drawn by the game UI shares: one GameUi, one target
// at the design resolution, and the capture that crops the render to its
// content and fits it into the thumbnail with the aspect kept. Nothing
// from the scene pipeline.
class UiSubject : public AssetThumbnailRenderer::Subject {
public:
    void shutdown(const AssetThumbnailRenderer::Deps& /*deps*/) override {
        if (ui_) {
            ui_->destroy();
            ui_.reset();
        }
        target_.reset();
    }

protected:
    // The UI and a target sized to the project's design frame, ready to load
    // into. Returns the frame size.
    VkExtent2D prepare(const AssetThumbnailRenderer::Deps& deps) {
        auto& vk = *deps.vulkan_context;
        if (!ui_) {
            ui_ = std::make_unique<GameUi>();
            vk.init_game_ui(*ui_, OffscreenPass::COLOR_FORMAT);
            ui_->set_resource_manager(deps.resource_manager);
        }
        ui_->set_project(deps.project);

        uint32_t frame_w = 1920;
        uint32_t frame_h = 1080;
        if (deps.project) {
            frame_w = deps.project->config().ui.design_width;
            frame_h = deps.project->config().ui.design_height;
        }
        if (!target_ || target_->extent().width != frame_w || target_->extent().height != frame_h) {
            target_ = std::make_unique<OffscreenPass>(vk.device_ref(), vk.allocator(),
                                                      frame_w, frame_h, VK_SAMPLE_COUNT_1_BIT);
        }
        return {frame_w, frame_h};
    }

    // Lays the loaded tree out in the frame, renders it and fills `out`.
    bool capture(const AssetThumbnailRenderer::Deps& deps, VkExtent2D frame,
                 AssetThumbnailRenderer::Pixels& out) {
        auto& vk = *deps.vulkan_context;
        const uint32_t frame_w = frame.width;
        const uint32_t frame_h = frame.height;
        // Laid out in the design frame at the design size, so layout rects
        // are target pixels.
        ui_->set_design_frame({static_cast<float>(frame_w), static_cast<float>(frame_h)});
        ui_->update(static_cast<float>(frame_w), static_cast<float>(frame_h),
                    vk.current_frame_index(), 0.0F, nullptr);
        vk.render_game_ui_preview(*ui_, *target_);

        // Crop to the content, read it back near its own size, and let the
        // CPU do the real shrink: one linear blit aliases past about 2:1.
        VkRect2D crop = content_bounds(*ui_->root(), frame_w, frame_h);
        constexpr uint32_t READBACK_MAX = 1024;
        float read_scale = std::min(1.0F, static_cast<float>(READBACK_MAX) /
                                              static_cast<float>(std::max(crop.extent.width, crop.extent.height)));
        auto read_w = std::max(1U, static_cast<uint32_t>(std::lround(static_cast<float>(crop.extent.width) * read_scale)));
        auto read_h = std::max(1U, static_cast<uint32_t>(std::lround(static_cast<float>(crop.extent.height) * read_scale)));
        ImageReadback readback({.device = vk.device(), .allocator = vk.allocator(),
                                .command_pool = vk.command_pool(), .queue = vk.graphics_queue()},
                               target_->color_image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               crop, read_w, read_h);

        constexpr uint32_t SIZE = AssetThumbnailRenderer::SIZE;
        float fit = std::min(1.0F, std::min(static_cast<float>(SIZE) / static_cast<float>(read_w),
                                            static_cast<float>(SIZE) / static_cast<float>(read_h)));
        auto fit_w = std::max(1U, static_cast<uint32_t>(std::lround(static_cast<float>(read_w) * fit)));
        auto fit_h = std::max(1U, static_cast<uint32_t>(std::lround(static_cast<float>(read_h) * fit)));
        std::vector<uint8_t> fitted(static_cast<size_t>(fit_w) * fit_h * 4);
        if (fit_w == read_w && fit_h == read_h) {
            std::memcpy(fitted.data(), readback.pixels(), fitted.size());
        } else {
            stbir_resize_uint8_srgb(readback.pixels(), static_cast<int>(read_w), static_cast<int>(read_h), 0,
                                    fitted.data(), static_cast<int>(fit_w), static_cast<int>(fit_h), 0,
                                    STBIR_RGBA);
        }

        // Centred on a transparent square, so a wide bar and a small icon
        // both sit in the cell at their own proportions.
        out.width = SIZE;
        out.height = SIZE;
        out.rgba.assign(static_cast<size_t>(SIZE) * SIZE * 4, 0);
        uint32_t ox = (SIZE - fit_w) / 2;
        uint32_t oy = (SIZE - fit_h) / 2;
        for (uint32_t y = 0; y < fit_h; ++y) {
            std::memcpy(out.rgba.data() + ((static_cast<size_t>(oy + y) * SIZE + ox) * 4),
                        fitted.data() + (static_cast<size_t>(y) * fit_w * 4),
                        static_cast<size_t>(fit_w) * 4);
        }
        return true;
    }

    std::unique_ptr<GameUi> ui_;
    std::unique_ptr<OffscreenPass> target_;
};

// A widget or layout rendered as the game draws it.
class WidgetSubject final : public UiSubject {
public:
    [[nodiscard]] bool handles(const fs::path& path) const override {
        auto ext = path.extension();
        return ext == ".fjwidget" || ext == ".fjui";
    }

    [[nodiscard]] fs::file_time_type source_time(const fs::path& path,
                                                 const Project* /*project*/) const override {
        auto newest = file_time(path);
        if (newest == fs::file_time_type::min()) return newest;
        return std::max(newest, file_time(stylesheet_for(path)));
    }

    bool render(const AssetThumbnailRenderer::Deps& deps, AssetThumbnailRenderer::SceneStage& /*stage*/,
                float /*dt*/, const std::string& path,
                AssetThumbnailRenderer::Pixels& out) override {
        auto frame = prepare(deps);

        fs::path markup(path);
        auto fjss = stylesheet_for(markup).string();
        if (markup.extension() == ".fjwidget") {
            auto asset = load_widget_asset(path);
            if (!asset) {
                FJELL_CORE_WARN("Widget thumbnail: {}", asset.error());
                return false;
            }
            if (!asset->markup_root) return false;
            // Declared prop defaults stand in for the values a game would
            // bind, the way the UI editor's preview shows them. They go in
            // before the load, which is when material="{prop}" is resolved,
            // and replace whatever the previous thumbnail left behind.
            ui_->context().clear();
            for (const auto& prop : asset->props) {
                if (!prop.default_value.empty()) ui_->context().set(prop.name, prop.default_value);
            }
            ui_->load_from_tree(UiDataBind::clone_node(*asset->markup_root, nullptr), fjss,
                                markup.parent_path().string(), path);
        } else {
            ui_->load(path, fjss);
        }
        if (!ui_->loaded()) return false;
        return capture(deps, frame, out);
    }
};

// A font recipe shown as a sample line in that font, drawn by the game UI
// through a sheet built here that names the recipe with @font-face.
class FontSubject final : public UiSubject {
public:
    [[nodiscard]] bool handles(const fs::path& path) const override {
        return path.extension() == ".fjfont";
    }

    [[nodiscard]] fs::file_time_type source_time(const fs::path& path,
                                                 const Project* /*project*/) const override {
        auto newest = file_time(path);
        if (newest == fs::file_time_type::min()) return newest;
        if (auto recipe = load_font_recipe(path)) {
            newest = std::max(newest, file_time(font_source_path(path, *recipe)));
        }
        return newest;
    }

    // The atlas comes from the shared library, which loads off the frame;
    // asking for it is what starts the load.
    [[nodiscard]] bool ready(const AssetThumbnailRenderer::Deps& deps,
                             const std::string& path) override {
        auto& fonts = deps.vulkan_context->font_library();
        switch (fonts.status(path)) {
            case FontLibrary::Status::unknown:
                (void)fonts.get(path);
                return false;
            case FontLibrary::Status::pending:
                return false;
            case FontLibrary::Status::ready:
            case FontLibrary::Status::failed:
                return true;
        }
        return true;
    }

    bool render(const AssetThumbnailRenderer::Deps& deps, AssetThumbnailRenderer::SceneStage& /*stage*/,
                float /*dt*/, const std::string& path,
                AssetThumbnailRenderer::Pixels& out) override {
        if (deps.vulkan_context->font_library().status(path) != FontLibrary::Status::ready) {
            return false;
        }
        auto frame = prepare(deps);
        // Large enough to read at thumbnail size; the capture crops to it.
        load_font_sample(*ui_, path, "Ag 123", 160.0f,
                         deps.vulkan_context->font_library().get(path));
        if (!ui_->loaded()) return false;
        return capture(deps, frame, out);
    }
};

} // namespace

// ── Public API ───────────────────────────────────────────────────────────

AssetThumbnailRenderer::AssetThumbnailRenderer() {
    subjects_.push_back(std::make_unique<MaterialSubject>());
    subjects_.push_back(std::make_unique<MeshSubject>());
    subjects_.push_back(std::make_unique<SkeletalMeshSubject>());
    subjects_.push_back(std::make_unique<AnimationClipSubject>());
    subjects_.push_back(std::make_unique<VfxSubject>());
    subjects_.push_back(std::make_unique<SceneFileSubject>());
    subjects_.push_back(std::make_unique<WidgetSubject>());
    subjects_.push_back(std::make_unique<FontSubject>());
}

AssetThumbnailRenderer::~AssetThumbnailRenderer() = default;

bool AssetThumbnailRenderer::supports(const fs::path& path) const {
    return subject_for(path) != nullptr;
}

bool AssetThumbnailRenderer::regenerates(const fs::path& path) const {
    const auto* subject = subject_for(path);
    return subject != nullptr && subject->renders_on_demand();
}

void AssetThumbnailRenderer::capture_scene(Scene& scene, const std::string& fjell_path,
                                           const View& view) {
    pending_capture_ = PendingCapture{.scene = &scene, .path = fjell_path, .view = view};
}

AssetThumbnailRenderer::Subject* AssetThumbnailRenderer::subject_for(const fs::path& path) const {
    for (const auto& subject : subjects_) {
        if (subject->handles(path)) return subject.get();
    }
    return nullptr;
}

void AssetThumbnailRenderer::request(const std::string& path) {
    if (entries_.contains(path)) return;
    refresh(path);
}

void AssetThumbnailRenderer::refresh(const std::string& path) {
    if (!supports(path)) return;
    if (queued_.insert(path).second) {
        queue_.push_back(path);
    }
}

void AssetThumbnailRenderer::refresh_all() {
    for (const auto& [path, entry] : entries_) refresh(path);
}

VkDescriptorSet AssetThumbnailRenderer::descriptor(const std::string& path) {
    auto it = entries_.find(path);
    if (it == entries_.end()) return VK_NULL_HANDLE;
    it->second.last_used = ticks_;
    return it->second.descriptor;
}

void AssetThumbnailRenderer::invalidate(const std::string& path) {
    std::error_code ec;
    fs::remove(cache_path(path), ec);

    auto it = entries_.find(path);
    if (it == entries_.end()) return;
    if (it->second.descriptor != VK_NULL_HANDLE) {
        // The icon cache frees retired entries at the next frame boundary;
        // without a Deps here the entry is handed over on the next tick.
        pending_retire_.push_back(it->second.descriptor_entry);
    }
    entries_.erase(it);
}

void AssetThumbnailRenderer::tick(const Deps& deps, float dt) {
    if (!deps.vulkan_context || !deps.resource_manager || !deps.icon_cache || !deps.thread_pool) return;

    ++ticks_;
    for (auto& entry : pending_retire_) deps.icon_cache->retire(entry);
    pending_retire_.clear();
    evict_least_recent(deps);

    if (stage_.draining()) {
        stage_.advance_drain();
        return;
    }

    if (pending_capture_) {
        render_pending_capture(deps, dt);
        if (stage_.draining()) return;
    }

    int served = 0;
    while (!queue_.empty() && served < CACHE_HITS_PER_TICK) {
        std::string path = std::move(queue_.front());
        queue_.pop_front();
        queued_.erase(path);

        auto* subject = subject_for(path);
        if (subject == nullptr) continue;

        auto newest = subject->source_time(path, deps.project);
        if (auto it = entries_.find(path); it != entries_.end()) {
            if (it->second.source_time >= newest) continue;
            retire(deps, it->second);
            entries_.erase(it);
        }

        if (serve_from_cache(deps, path, newest)) {
            ++served;
            continue;
        }

        // A render owns the rest of this tick. When the stage was just
        // created the render waits for the drain, so the path goes back on
        // the queue rather than being lost.
        (void)stage_.ensure(deps);
        if (stage_.draining() || !subject->ready(deps, path)) {
            refresh(path);
            return;
        }
        if (!render(deps, dt, *subject, path, newest)) {
            entries_[path] = Entry{.source_time = newest, .failed = true};
        }
        return;
    }
}

void AssetThumbnailRenderer::shutdown(const Deps& deps) {
    for (auto& [path, entry] : entries_) retire(deps, entry);
    entries_.clear();
    for (auto& entry : pending_retire_) deps.icon_cache->retire(entry);
    pending_retire_.clear();
    queue_.clear();
    queued_.clear();

    for (auto& subject : subjects_) subject->shutdown(deps);
    stage_.shutdown(deps);
}

// ── Cache ────────────────────────────────────────────────────────────────

fs::path AssetThumbnailRenderer::cache_path(const fs::path& asset) {
    // The full filename keeps an asset apart from a texture of the same
    // stem: the icon cache names those `<stem>.thumb.png`.
    return asset.parent_path() / ".fjcache" / (asset.filename().string() + ".thumb.png");
}

bool AssetThumbnailRenderer::serve_from_cache(const Deps& deps, const std::string& path,
                                              fs::file_time_type newest) {
    auto cached = cache_path(path);
    auto cache_time = file_time(cached);
    if (cache_time == fs::file_time_type::min() || cache_time < newest) return false;

    int w = 0;
    int h = 0;
    int channels = 0;
    auto* pixels = stbi_load(cached.string().c_str(), &w, &h, &channels, 4);
    if (pixels == nullptr) return false;
    auto entry = deps.icon_cache->upload_rgba(pixels, w, h, "asset thumbnail " + path);
    stbi_image_free(pixels);
    return store(deps, path, entry, newest);
}

// ── Scene stage ──────────────────────────────────────────────────────────

bool AssetThumbnailRenderer::SceneStage::ensure(const Deps& deps) {
    if (target_) return true;
    auto& vk = *deps.vulkan_context;

    // The graph decides both which passes run and which per-viewport images
    // the post-process resources allocate. A missing or broken graph is
    // reported, not papered over with a hand-rolled config.
    graph_ = vk.get_or_load_graph(GRAPH_PATH);
    if (graph_ == nullptr) {
        FJELL_CORE_ERROR("Scene thumbnails disabled: failed to load {}", GRAPH_PATH);
        return false;
    }

    // Pipelines are compiled for the main viewport's sample count.
    target_ = std::make_unique<OffscreenPass>(vk.device_ref(), vk.allocator(),
                                              SIZE, SIZE, vk.msaa_samples());
    PostProcessResources::PassRefs refs{
        vk.hiz_pass(), vk.gtao_pass(), vk.volumetric_fog_pass(),
        vk.post_pass(), vk.ddgi_pass(), vk.ss_gi_pass(), vk.taa_resolve_pass(),
        vk.rt_reflection_pass()};
    pp_ = std::make_unique<PostProcessResources>(vk.device(), vk.allocator(), SIZE, SIZE,
                                                 pp_config_from_graph(*graph_), refs);
    global_set_ = vk.gpu().global_bindings().allocate_set();
    skip_frames_ = DRAIN_FRAMES;
    return true;
}

Scene& AssetThumbnailRenderer::SceneStage::begin() {
    // One fixed light and the subject: the thumbnail's lighting never
    // follows whatever scene happens to be open.
    scene_.clear();
    auto sun = std::make_unique<DirectionalLightNode>();
    sun->name = "Thumbnail Sun";
    sun->direction = glm::normalize(glm::vec3{-0.45F, -0.8F, -0.4F});
    scene_.add_node(std::move(sun));
    return scene_;
}

bool AssetThumbnailRenderer::SceneStage::render(const Deps& deps, float dt, const Framing& framing, Pixels* out,
                                                const FjGraph* graph) {
    Camera camera(framing.center,
                  std::max(framing.radius * CAMERA_DISTANCE_PER_RADIUS, CAMERA_MIN_DISTANCE),
                  CAMERA_YAW, CAMERA_PITCH);
    View view{.view = camera.view_matrix(),
              .projection = camera.projection_matrix(1.0F),
              .position = camera.position()};
    return render_view(deps, dt, scene_, view, out, RenderOptions{.graph = graph});
}

bool AssetThumbnailRenderer::SceneStage::render_view(const Deps& deps, float dt, Scene& scene, const View& view,
                                                     Pixels* out, const RenderOptions& options) {
    if (!target_ || !pp_) return false;
    auto& vk = *deps.vulkan_context;
    scene.compute_world_matrices(nullptr);

    ViewportRenderRequest req;
    req.target = target_.get();
    req.view = view.view;
    req.projection = view.projection;
    req.camera_position = view.position;
    req.pp_resources = pp_.get();
    req.skip_shadows = options.skip_shadows;
    req.viewport_graph = options.graph != nullptr ? options.graph : graph_;
    req.global_set = global_set_;
    req.backdrop = options.backdrop;

    auto& post = vk.post_pass();
    const float saved_exposure = post.exposure();
    const bool saved_auto = post.auto_exposure();
    if (options.pin_exposure) {
        post.set_auto_exposure(false);
        post.set_exposure(0.0F);
    }
    // The stage's own scene carries its own sun; the environment comes from it,
    // never from whatever scene the editor has open.
    const EnvironmentState environment = resolve_environment(scene);
    vk.render_preview(req, environment, &scene, dt, 0, *deps.thread_pool);
    post.set_exposure(saved_exposure);
    post.set_auto_exposure(saved_auto);
    if (out == nullptr) return true;

    auto* output = pp_->post();
    if (output == nullptr || output->image == VK_NULL_HANDLE) {
        FJELL_CORE_ERROR("Scene thumbnail: post output unavailable");
        return false;
    }
    ImageReadback readback({.device = vk.device(), .allocator = vk.allocator(),
                            .command_pool = vk.command_pool(), .queue = vk.graphics_queue()},
                           output->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           output->extent.width, output->extent.height);
    out->width = readback.width();
    out->height = readback.height();
    out->rgba.assign(readback.pixels(),
                     readback.pixels() + static_cast<size_t>(out->width) * out->height * 4);
    return true;
}

void AssetThumbnailRenderer::SceneStage::shutdown(const Deps& deps) {
    if (deps.vulkan_context) deps.vulkan_context->release_scene_particles(scene_);
    if (global_set_ != VK_NULL_HANDLE && deps.vulkan_context) {
        deps.vulkan_context->gpu().global_bindings().free_set(global_set_);
        global_set_ = VK_NULL_HANDLE;
    }
    pp_.reset();
    target_.reset();
    scene_.clear();
}

bool AssetThumbnailRenderer::SceneSubject::render(const Deps& deps, SceneStage& stage,
                                                  float dt,
                                                  const std::string& path, Pixels& out) {
    if (!stage.ensure(deps)) return false;
    Framing framing;
    if (!populate(stage.begin(), deps, path, framing)) return false;
    return stage.render(deps, dt, framing, &out);
}

// ── Rendering ────────────────────────────────────────────────────────────

bool AssetThumbnailRenderer::render(const Deps& deps, float dt,
                                    Subject& subject, const std::string& path,
                                    fs::file_time_type newest) {
    Pixels pixels;
    if (!subject.render(deps, stage_, dt, path, pixels)) return false;
    return store_pixels(deps, path, pixels, newest);
}

void AssetThumbnailRenderer::render_pending_capture(const Deps& deps, float dt) {
    if (!stage_.ensure(deps) || stage_.draining()) return;
    PendingCapture capture = std::move(*pending_capture_);
    pending_capture_.reset();

    const auto* graph = deps.vulkan_context->get_or_load_graph(SCENE_GRAPH_PATH);
    if (graph == nullptr) {
        FJELL_CORE_ERROR("Scene thumbnail: failed to load {}", SCENE_GRAPH_PATH);
        return;
    }
    Pixels pixels;
    if (!stage_.render_view(deps, dt, *capture.scene, capture.view, &pixels,
                            RenderOptions{.graph = graph, .backdrop = nullptr,
                                          .skip_shadows = false, .pin_exposure = false})) {
        return;
    }
    auto* subject = subject_for(capture.path);
    auto newest = subject != nullptr ? subject->source_time(capture.path, deps.project)
                                     : file_time(capture.path);
    if (auto it = entries_.find(capture.path); it != entries_.end()) {
        retire(deps, it->second);
        entries_.erase(it);
    }
    store_pixels(deps, capture.path, pixels, newest);
}

bool AssetThumbnailRenderer::store_pixels(const Deps& deps, const std::string& path,
                                          const Pixels& pixels, fs::file_time_type newest) {
    if (pixels.width == 0 || pixels.height == 0 ||
        pixels.rgba.size() < static_cast<size_t>(pixels.width) * pixels.height * 4) {
        return false;
    }

    auto cached = cache_path(path);
    std::error_code ec;
    fs::create_directories(cached.parent_path(), ec);
    if (!ec) {
        int w = static_cast<int>(pixels.width);
        int h = static_cast<int>(pixels.height);
        if (stbi_write_png(cached.string().c_str(), w, h, 4, pixels.rgba.data(), w * 4) == 0) {
            FJELL_CORE_WARN("Asset thumbnail: failed to write {}", cached.string());
        }
    }

    auto entry = deps.icon_cache->upload_rgba(pixels.rgba.data(), static_cast<int>(pixels.width),
                                              static_cast<int>(pixels.height),
                                              "asset thumbnail " + path);
    return store(deps, path, entry, newest);
}

bool AssetThumbnailRenderer::store(const Deps& deps, const std::string& path,
                                   IconCache::IconEntry entry, fs::file_time_type newest) {
    // The image exists either way; without a descriptor nothing can draw
    // it, so it goes straight back rather than sitting unreachable.
    if (entry.descriptor == VK_NULL_HANDLE) {
        FJELL_CORE_ERROR("Asset thumbnail: ImGui could not register {}", path);
        deps.icon_cache->retire(entry);
        return false;
    }
    entries_[path] = Entry{
        .descriptor = entry.descriptor,
        .descriptor_entry = entry,
        .source_time = newest,
        .last_used = ticks_,
    };
    return true;
}

void AssetThumbnailRenderer::retire(const Deps& deps, Entry& entry) {
    if (entry.descriptor_entry.image == VK_NULL_HANDLE) return;
    deps.icon_cache->retire(entry.descriptor_entry);
    entry.descriptor_entry = {};
    entry.descriptor = VK_NULL_HANDLE;
}

void AssetThumbnailRenderer::evict_least_recent(const Deps& deps) {
    // Only entries with an image count; failed ones hold nothing. An entry
    // handed out this tick is on screen and stays.
    size_t resident = 0;
    for (const auto& [path, entry] : entries_) {
        if (entry.descriptor_entry.image != VK_NULL_HANDLE) ++resident;
    }
    while (resident > MAX_RESIDENT) {
        auto oldest = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.descriptor_entry.image == VK_NULL_HANDLE) continue;
            if (it->second.last_used >= ticks_) continue;
            if (oldest == entries_.end() || it->second.last_used < oldest->second.last_used) {
                oldest = it;
            }
        }
        if (oldest == entries_.end()) return;
        retire(deps, oldest->second);
        entries_.erase(oldest);
        --resident;
    }
}

} // namespace fjell
