#include "ui/material_thumbnail_renderer.hpp"

#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/project.hpp"
#include "renderer/camera.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/gpu/image_readback.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/tonemap_pass.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/material_asset.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/viewport_source.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/light_node.hpp"
#include "ui/icon_cache.hpp"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <vector>

namespace fjell {

namespace fs = std::filesystem;

namespace {

constexpr const char* GRAPH_PATH = "assets/render_graphs/material_thumbnail.fjgraph";

// The camera preview waits this long too: MAX_FRAMES_IN_FLIGHT command
// buffers may still be pending when the private set is allocated.
constexpr int DRAIN_FRAMES = 3;

// Cache hits are a PNG decode and a small upload each, so several fit in a
// frame; a render is a full pipeline submit and gets the frame to itself.
constexpr int CACHE_HITS_PER_TICK = 8;

// Three-quarter view of the unit-diameter sphere, filling most of the frame.
constexpr float CAMERA_DISTANCE = 1.55f;
constexpr float CAMERA_YAW = -30.0f;
constexpr float CAMERA_PITCH = 20.0f;

} // namespace

// ── Public API ───────────────────────────────────────────────────────────

void MaterialThumbnailRenderer::request(const std::string& fjmat_path) {
    if (entries_.contains(fjmat_path)) return;
    refresh(fjmat_path);
}

void MaterialThumbnailRenderer::refresh(const std::string& fjmat_path) {
    if (queued_.insert(fjmat_path).second) {
        queue_.push_back(fjmat_path);
    }
}

VkDescriptorSet MaterialThumbnailRenderer::descriptor(const std::string& fjmat_path) const {
    auto it = entries_.find(fjmat_path);
    return it != entries_.end() ? it->second.descriptor : VK_NULL_HANDLE;
}

void MaterialThumbnailRenderer::invalidate(const std::string& fjmat_path) {
    std::error_code ec;
    fs::remove(cache_path(fjmat_path), ec);

    auto it = entries_.find(fjmat_path);
    if (it == entries_.end()) return;
    if (it->second.descriptor != VK_NULL_HANDLE) {
        // The icon cache frees retired entries at the next frame boundary;
        // without a Deps here the entry is handed over on the next tick.
        pending_retire_.push_back(it->second.descriptor_entry);
    }
    entries_.erase(it);
}

void MaterialThumbnailRenderer::tick(const Deps& deps, const DirectionalLight& light, float dt) {
    if (!deps.vulkan_context || !deps.resource_manager || !deps.icon_cache || !deps.thread_pool) return;

    for (auto& entry : pending_retire_) deps.icon_cache->retire(entry);
    pending_retire_.clear();

    if (skip_frames_ > 0) {
        --skip_frames_;
        return;
    }

    int served = 0;
    while (!queue_.empty() && served < CACHE_HITS_PER_TICK) {
        std::string path = std::move(queue_.front());
        queue_.pop_front();
        queued_.erase(path);

        auto newest = source_time(path, deps.project);
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
        if (!render(deps, light, dt, path, newest)) {
            entries_[path] = Entry{.source_time = newest, .failed = true};
        }
        return;
    }
}

void MaterialThumbnailRenderer::shutdown(const Deps& deps) {
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
    sphere_id_ = 0;
}

// ── Cache ────────────────────────────────────────────────────────────────

fs::path MaterialThumbnailRenderer::cache_path(const fs::path& fjmat) {
    // The full filename keeps a material apart from a texture of the same
    // stem: the icon cache names those `<stem>.thumb.png`.
    return fjmat.parent_path() / ".fjcache" / (fjmat.filename().string() + ".thumb.png");
}

fs::file_time_type MaterialThumbnailRenderer::source_time(const fs::path& fjmat,
                                                          const Project* project) {
    std::error_code ec;
    auto newest = fs::last_write_time(fjmat, ec);
    if (ec) return fs::file_time_type::min();

    // The look also depends on the shader and the textures, so an edit to
    // any of them is a newer source. A reference that does not resolve is
    // ignored: a broken material still gets one thumbnail, not one per frame.
    MaterialAsset asset;
    try {
        asset = load_material_asset(fjmat.string());
    } catch (const std::exception&) {
        return newest;
    }
    auto consider = [&](const std::string& reference) {
        if (reference.empty() || reference.starts_with("$viewport:")) return;
        auto resolved = base_path::resolve_asset(reference, project);
        if (resolved.empty()) return;
        std::error_code tec;
        auto t = fs::last_write_time(resolved, tec);
        if (!tec) newest = std::max(newest, t);
    };
    consider(asset.shader);
    for (const auto& [slot, path] : asset.textures) consider(path);
    return newest;
}

bool MaterialThumbnailRenderer::serve_from_cache(const Deps& deps, const std::string& fjmat_path,
                                                 fs::file_time_type newest) {
    auto cached = cache_path(fjmat_path);
    std::error_code ec;
    auto cache_time = fs::last_write_time(cached, ec);
    if (ec || cache_time < newest) return false;

    int w = 0, h = 0, channels = 0;
    auto* pixels = stbi_load(cached.string().c_str(), &w, &h, &channels, 4);
    if (!pixels) return false;
    auto entry = deps.icon_cache->upload_rgba(pixels, w, h);
    stbi_image_free(pixels);

    entries_[fjmat_path] = Entry{
        .descriptor = entry.descriptor,
        .descriptor_entry = entry,
        .source_time = newest,
    };
    return true;
}

// ── Rendering ────────────────────────────────────────────────────────────

void MaterialThumbnailRenderer::ensure_render_resources(const Deps& deps) {
    if (target_) return;
    auto& vk = *deps.vulkan_context;

    // The graph decides both which passes run and which per-viewport images
    // the post-process resources allocate. A missing or broken graph is
    // reported, not papered over with a hand-rolled config.
    graph_ = vk.get_or_load_graph(GRAPH_PATH);
    if (graph_ == nullptr) {
        FJELL_CORE_ERROR("Material thumbnails disabled: failed to load {}", GRAPH_PATH);
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

    // One sphere and one fixed light: the thumbnail's lighting never
    // follows whatever scene happens to be open.
    scene_.clear();
    auto sphere = deps.resource_manager->create_primitive("sphere");
    if (!sphere) {
        FJELL_CORE_ERROR("Material thumbnails disabled: {}", sphere.error());
        target_.reset();
        return;
    }
    sphere_id_ = scene_.add(*sphere, MaterialHandle{}, *deps.resource_manager,
                            {}, "Thumbnail Sphere", "primitive:sphere");
    auto sun = std::make_unique<DirectionalLightNode>();
    sun->name = "Thumbnail Sun";
    sun->direction = glm::normalize(glm::vec3{-0.45f, -0.8f, -0.4f});
    scene_.add_node(std::move(sun));
    scene_.compute_world_matrices(nullptr);
}

bool MaterialThumbnailRenderer::render(const Deps& deps, const DirectionalLight& light, float dt,
                                       const std::string& fjmat_path, fs::file_time_type newest) {
    if (!target_ || !pp_ || sphere_id_ == 0) return false;
    auto& vk = *deps.vulkan_context;
    auto& resources = *deps.resource_manager;

    auto handle = resources.load_material_asset(fjmat_path, deps.project);
    if (!handle) {
        FJELL_CORE_WARN("Material thumbnail: {}", handle.error());
        return false;
    }
    auto* material = resources.get(*handle);
    auto* node = scene_.find(sphere_id_);
    auto* mesh = node ? node->components.get<MeshComponent>() : nullptr;
    if (!material || !mesh) return false;

    mesh->material_handle = *handle;
    mesh->material = material;
    mesh->material_name = fjmat_path;
    scene_.compute_world_matrices(nullptr);

    Camera camera({0.0f, 0.0f, 0.0f}, CAMERA_DISTANCE, CAMERA_YAW, CAMERA_PITCH);
    ViewportRenderRequest req;
    req.target = target_.get();
    req.view = camera.view_matrix();
    req.projection = camera.projection_matrix(1.0f);
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
    tonemap.set_exposure(0.0f);
    vk.render_preview(req, light, &scene_, dt, 0, *deps.thread_pool);
    tonemap.set_exposure(saved_exposure);
    tonemap.set_auto_exposure(saved_auto);

    auto* output = pp_->tonemap();
    if (output == nullptr || output->image == VK_NULL_HANDLE) {
        FJELL_CORE_ERROR("Material thumbnail: tonemap output unavailable");
        return false;
    }
    ImageReadback readback({.device = vk.device(), .allocator = vk.allocator(),
                            .command_pool = vk.command_pool(), .queue = vk.graphics_queue()},
                           output->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           output->extent.width, output->extent.height);

    auto cached = cache_path(fjmat_path);
    std::error_code ec;
    fs::create_directories(cached.parent_path(), ec);
    if (!ec) {
        int w = static_cast<int>(readback.width());
        int h = static_cast<int>(readback.height());
        if (stbi_write_png(cached.string().c_str(), w, h, 4, readback.pixels(), w * 4) == 0) {
            FJELL_CORE_WARN("Material thumbnail: failed to write {}", cached.string());
        }
    }

    auto entry = deps.icon_cache->upload_rgba(readback.pixels(), static_cast<int>(readback.width()),
                                              static_cast<int>(readback.height()));
    entries_[fjmat_path] = Entry{
        .descriptor = entry.descriptor,
        .descriptor_entry = entry,
        .source_time = newest,
    };
    return true;
}

void MaterialThumbnailRenderer::retire(const Deps& deps, Entry& entry) {
    if (entry.descriptor == VK_NULL_HANDLE) return;
    deps.icon_cache->retire(entry.descriptor_entry);
    entry.descriptor = VK_NULL_HANDLE;
}

} // namespace fjell
