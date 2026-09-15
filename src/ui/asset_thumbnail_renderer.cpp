#include "ui/asset_thumbnail_renderer.hpp"

#include "animation/animation_clip.hpp"
#include "animation/animator_component.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project.hpp"
#include "renderer/camera.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/gpu/image_readback.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/tonemap_pass.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resources/fjmesh_writer.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/material_asset.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/viewport_source.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/light_node.hpp"
#include "ui/icon_cache.hpp"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <optional>

namespace fjell {

namespace fs = std::filesystem;

namespace {

constexpr const char* GRAPH_PATH = "assets/render_graphs/asset_thumbnail.fjgraph";

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

class MaterialSubject final : public AssetThumbnailRenderer::Subject {
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
        scene.add(*sphere, *material, resources, {}, "Thumbnail Sphere", "primitive:sphere", path);
        // The primitive is a unit-diameter sphere at the origin.
        framing = {.center = {0.0F, 0.0F, 0.0F}, .radius = 0.5F};
        return true;
    }
};

class MeshSubject final : public AssetThumbnailRenderer::Subject {
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

class SkeletalMeshSubject final : public AssetThumbnailRenderer::Subject {
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

class AnimationClipSubject final : public AssetThumbnailRenderer::Subject {
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

} // namespace

// ── Public API ───────────────────────────────────────────────────────────

AssetThumbnailRenderer::AssetThumbnailRenderer() {
    subjects_.push_back(std::make_unique<MaterialSubject>());
    subjects_.push_back(std::make_unique<MeshSubject>());
    subjects_.push_back(std::make_unique<SkeletalMeshSubject>());
    subjects_.push_back(std::make_unique<AnimationClipSubject>());
}

AssetThumbnailRenderer::~AssetThumbnailRenderer() = default;

bool AssetThumbnailRenderer::supports(const fs::path& path) const {
    return subject_for(path) != nullptr;
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

void AssetThumbnailRenderer::tick(const Deps& deps, const DirectionalLight& light, float dt) {
    if (!deps.vulkan_context || !deps.resource_manager || !deps.icon_cache || !deps.thread_pool) return;

    ++ticks_;
    for (auto& entry : pending_retire_) deps.icon_cache->retire(entry);
    pending_retire_.clear();
    evict_least_recent(deps);

    if (skip_frames_ > 0) {
        --skip_frames_;
        return;
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

        // A render owns the rest of this tick. When the resources were just
        // created the render waits for the drain, so the path goes back on
        // the queue rather than being lost.
        ensure_render_resources(deps);
        if (skip_frames_ > 0) {
            refresh(path);
            return;
        }
        if (!render(deps, light, dt, *subject, path, newest)) {
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

    if (global_set_ != VK_NULL_HANDLE && deps.vulkan_context) {
        deps.vulkan_context->gpu().global_bindings().free_set(global_set_);
        global_set_ = VK_NULL_HANDLE;
    }
    pp_.reset();
    target_.reset();
    scene_.clear();
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

// ── Rendering ────────────────────────────────────────────────────────────

void AssetThumbnailRenderer::ensure_render_resources(const Deps& deps) {
    if (target_) return;
    auto& vk = *deps.vulkan_context;

    // The graph decides both which passes run and which per-viewport images
    // the post-process resources allocate. A missing or broken graph is
    // reported, not papered over with a hand-rolled config.
    graph_ = vk.get_or_load_graph(GRAPH_PATH);
    if (graph_ == nullptr) {
        FJELL_CORE_ERROR("Asset thumbnails disabled: failed to load {}", GRAPH_PATH);
        return;
    }

    // Pipelines are compiled for the main viewport's sample count.
    target_ = std::make_unique<OffscreenPass>(vk.device_ref(), vk.allocator(),
                                              SIZE, SIZE, vk.msaa_samples());
    PostProcessResources::PassRefs refs{
        vk.hiz_pass(), vk.gtao_pass(), vk.volumetric_fog_pass(),
        vk.tonemap_pass(), vk.ddgi_pass(), vk.ss_gi_pass(), vk.taa_resolve_pass(),
        vk.rt_reflection_pass()};
    pp_ = std::make_unique<PostProcessResources>(vk.device(), vk.allocator(), SIZE, SIZE,
                                                 pp_config_from_graph(*graph_), refs);
    global_set_ = vk.gpu().global_bindings().allocate_set();
    skip_frames_ = DRAIN_FRAMES;
}

bool AssetThumbnailRenderer::render(const Deps& deps, const DirectionalLight& light, float dt,
                                    Subject& subject, const std::string& path,
                                    fs::file_time_type newest) {
    if (!target_ || !pp_) return false;
    auto& vk = *deps.vulkan_context;

    // One fixed light and the subject: the thumbnail's lighting never
    // follows whatever scene happens to be open.
    scene_.clear();
    auto sun = std::make_unique<DirectionalLightNode>();
    sun->name = "Thumbnail Sun";
    sun->direction = glm::normalize(glm::vec3{-0.45F, -0.8F, -0.4F});
    scene_.add_node(std::move(sun));

    Framing framing;
    if (!subject.populate(scene_, deps, path, framing)) return false;
    scene_.compute_world_matrices(nullptr);

    Camera camera(framing.center,
                  std::max(framing.radius * CAMERA_DISTANCE_PER_RADIUS, CAMERA_MIN_DISTANCE),
                  CAMERA_YAW, CAMERA_PITCH);
    ViewportRenderRequest req;
    req.target = target_.get();
    req.view = camera.view_matrix();
    req.projection = camera.projection_matrix(1.0F);
    req.camera_position = camera.position();
    req.pp_resources = pp_.get();
    req.skip_shadows = true;
    req.viewport_graph = graph_;
    req.global_set = global_set_;

    // Exposure is one adapted value shared by every viewport. Pin it for
    // this submit so the thumbnail does not depend on how bright the open
    // scene happened to be.
    auto& tonemap = vk.tonemap_pass();
    const float saved_exposure = tonemap.exposure();
    const bool saved_auto = tonemap.auto_exposure();
    tonemap.set_auto_exposure(false);
    tonemap.set_exposure(0.0F);
    vk.render_preview(req, light, &scene_, dt, 0, *deps.thread_pool);
    tonemap.set_exposure(saved_exposure);
    tonemap.set_auto_exposure(saved_auto);

    auto* output = pp_->tonemap();
    if (output == nullptr || output->image == VK_NULL_HANDLE) {
        FJELL_CORE_ERROR("Asset thumbnail: tonemap output unavailable");
        return false;
    }
    ImageReadback readback({.device = vk.device(), .allocator = vk.allocator(),
                            .command_pool = vk.command_pool(), .queue = vk.graphics_queue()},
                           output->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           output->extent.width, output->extent.height);

    auto cached = cache_path(path);
    std::error_code ec;
    fs::create_directories(cached.parent_path(), ec);
    if (!ec) {
        int w = static_cast<int>(readback.width());
        int h = static_cast<int>(readback.height());
        if (stbi_write_png(cached.string().c_str(), w, h, 4, readback.pixels(), w * 4) == 0) {
            FJELL_CORE_WARN("Asset thumbnail: failed to write {}", cached.string());
        }
    }

    auto entry = deps.icon_cache->upload_rgba(readback.pixels(), static_cast<int>(readback.width()),
                                              static_cast<int>(readback.height()),
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
