#pragma once

#include "renderer/uniform_types.hpp"
#include "scene/scene.hpp"
#include "ui/icon_cache.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fjell {

class OffscreenPass;
class PostProcessResources;
class Project;
class ResourceManager;
class ThreadPool;
class VulkanContext;
struct FjGraph;

/// Renders a `.fjmat` onto a lit sphere and hands the result out as an
/// ImGui texture. Each thumbnail is rendered once through the real material
/// pipeline, written to `.fjcache/<file>.thumb.png` next to the asset, and
/// served from that file on later runs until the material, its shader or
/// one of its textures is newer than the cache.
///
/// UI code only ever calls request() and descriptor(); the engine calls
/// tick() once per frame in the window where offscreen previews render,
/// after the frame fence and before the main command buffer is recorded.
/// One render per frame, since each is a blocking submit and readback.
class MaterialThumbnailRenderer {
public:
    static constexpr uint32_t SIZE = 128;

    struct Deps {
        VulkanContext* vulkan_context{nullptr};
        ResourceManager* resource_manager{nullptr};
        IconCache* icon_cache{nullptr};
        ThreadPool* thread_pool{nullptr};
        const Project* project{nullptr};
    };

    /// Queue a thumbnail for an absolute `.fjmat` path the renderer knows
    /// nothing about yet. Cheap and idempotent, so draw code may call it
    /// every frame a descriptor comes back null; a material that failed to
    /// render stays failed until refresh() or invalidate().
    void request(const std::string& fjmat_path);

    /// Queue a material whether or not it is known. Its files' timestamps
    /// are compared against the served thumbnail and it is re-rendered if
    /// any is newer. Meant for directory listings, not per-frame draw code:
    /// the check parses the `.fjmat`.
    void refresh(const std::string& fjmat_path);

    /// The thumbnail's ImGui descriptor, or VK_NULL_HANDLE while it is not
    /// ready. Never loads or renders.
    [[nodiscard]] VkDescriptorSet descriptor(const std::string& fjmat_path) const;

    /// Forget a thumbnail and delete its cache file. The descriptor is
    /// retired, not destroyed: a draw list recorded this frame may still
    /// hold it, so it is freed at the next frame boundary together with the
    /// icon cache's retired thumbnails.
    void invalidate(const std::string& fjmat_path);

    /// Serve or render at most one queued thumbnail. Call once per frame in
    /// the offscreen preview window.
    void tick(const Deps& deps, const DirectionalLight& light, float dt);

    /// Release GPU resources and retire every descriptor. Call after the
    /// device is idle and before the icon cache is destroyed.
    void shutdown(const Deps& deps);

    /// The engine's renderer, for asset-type preview callbacks that receive
    /// no editor dependencies. Null before the editor is up and after
    /// shutdown, the same way IconCache::instance() is.
    [[nodiscard]] static MaterialThumbnailRenderer* instance() { return s_instance; }
    static void set_instance(MaterialThumbnailRenderer* renderer) { s_instance = renderer; }

private:
    static inline MaterialThumbnailRenderer* s_instance{nullptr};

    struct Entry {
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
        // The image behind the descriptor, owned here until retired.
        IconCache::IconEntry descriptor_entry{};
        // Newest timestamp among the material and the files it references
        // when this entry was produced.
        std::filesystem::file_time_type source_time;
        bool failed{false};
    };

    [[nodiscard]] static std::filesystem::path cache_path(const std::filesystem::path& fjmat);
    [[nodiscard]] static std::filesystem::file_time_type source_time(
        const std::filesystem::path& fjmat, const Project* project);

    void ensure_render_resources(const Deps& deps);
    bool serve_from_cache(const Deps& deps, const std::string& fjmat_path,
                          std::filesystem::file_time_type newest);
    bool render(const Deps& deps, const DirectionalLight& light, float dt,
                const std::string& fjmat_path, std::filesystem::file_time_type newest);
    void retire(const Deps& deps, Entry& entry);

    std::deque<std::string> queue_;
    std::unordered_set<std::string> queued_;
    std::unordered_map<std::string, Entry> entries_;
    // Entries invalidated outside tick(), handed to the icon cache on the
    // next tick so they are freed at a frame boundary.
    std::vector<IconCache::IconEntry> pending_retire_;

    // Render resources, created on the first render.
    std::unique_ptr<OffscreenPass> target_;
    std::unique_ptr<PostProcessResources> pp_;
    const FjGraph* graph_{nullptr};
    VkDescriptorSet global_set_{VK_NULL_HANDLE};
    // Frames to wait after creating the resources so every in-flight command
    // buffer has drained before the private set is first bound.
    int skip_frames_{0};

    Scene scene_;
    uint32_t sphere_id_{0};
};

} // namespace fjell
