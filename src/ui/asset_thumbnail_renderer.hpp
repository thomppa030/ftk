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

/// Renders an asset into a small picture of itself and hands the result out
/// as an ImGui texture: a `.fjmat` on a lit sphere, a `.fjmesh` (or a glTF
/// with no cache yet) with its authored slot materials, a `.fjskel` in its
/// bind pose. Each thumbnail is rendered once through the real pipeline,
/// written to `.fjcache/<file>.thumb.png` next to the asset, and served from
/// that file on later runs until the asset or a file it references is newer.
///
/// UI code only ever calls request(), refresh() and descriptor(); the engine
/// calls tick() once per frame in the window where offscreen previews render,
/// after the frame fence and before the main command buffer is recorded. One
/// render per frame, since each is a blocking submit and readback.
class AssetThumbnailRenderer {
public:
    static constexpr uint32_t SIZE = 128;

    struct Deps {
        VulkanContext* vulkan_context{nullptr};
        ResourceManager* resource_manager{nullptr};
        IconCache* icon_cache{nullptr};
        ThreadPool* thread_pool{nullptr};
        const Project* project{nullptr};
    };

    /// Whether the renderer can draw this file, judged by extension.
    [[nodiscard]] static bool supports(const std::filesystem::path& path);

    /// Queue a thumbnail for an absolute asset path the renderer knows
    /// nothing about yet. Cheap and idempotent, so draw code may call it
    /// every frame a descriptor comes back null; an asset that failed to
    /// render stays failed until refresh() or invalidate(). Unsupported
    /// files are ignored.
    void request(const std::string& path);

    /// Queue an asset whether or not it is known. Its files' timestamps are
    /// compared against the served thumbnail and it is re-rendered if any is
    /// newer. Meant for directory listings, not per-frame draw code: the
    /// check reads the asset's references.
    void refresh(const std::string& path);

    /// The thumbnail's ImGui descriptor, or VK_NULL_HANDLE while it is not
    /// ready. Never loads or renders.
    [[nodiscard]] VkDescriptorSet descriptor(const std::string& path) const;

    /// Forget a thumbnail and delete its cache file. The descriptor is
    /// retired, not destroyed: a draw list recorded this frame may still
    /// hold it, so it is freed at the next frame boundary together with the
    /// icon cache's retired thumbnails.
    void invalidate(const std::string& path);

    /// Serve or render at most one queued thumbnail. Call once per frame in
    /// the offscreen preview window.
    void tick(const Deps& deps, const DirectionalLight& light, float dt);

    /// Release GPU resources and retire every descriptor. Call after the
    /// device is idle and before the icon cache is destroyed.
    void shutdown(const Deps& deps);

    /// The engine's renderer, for asset-type preview callbacks that receive
    /// no editor dependencies. Null before the editor is up and after
    /// shutdown, the same way IconCache::instance() is.
    [[nodiscard]] static AssetThumbnailRenderer* instance() { return s_instance; }
    static void set_instance(AssetThumbnailRenderer* renderer) { s_instance = renderer; }

    /// What one asset type contributes: the files whose timestamps date the
    /// thumbnail, and the nodes that stand in the scene. The renderer owns
    /// the light, the camera and the pipeline.
    struct Framing {
        glm::vec3 center{0.0f};
        float radius{0.5f};
    };
    class Subject {
    public:
        virtual ~Subject() = default;
        [[nodiscard]] virtual bool handles(const std::filesystem::path& path) const = 0;
        /// Newest timestamp among the asset and the files it references.
        [[nodiscard]] virtual std::filesystem::file_time_type source_time(
            const std::filesystem::path& path, const Project* project) const = 0;
        /// Add the asset to `scene` and say what to frame. False when the
        /// asset could not be loaded.
        virtual bool populate(Scene& scene, const Deps& deps, const std::string& path,
                              Framing& framing) = 0;
    };

private:
    static inline AssetThumbnailRenderer* s_instance{nullptr};

    struct Entry {
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
        // The image behind the descriptor, owned here until retired.
        IconCache::IconEntry descriptor_entry{};
        // Newest timestamp among the asset and the files it references when
        // this entry was produced.
        std::filesystem::file_time_type source_time;
        bool failed{false};
    };

    [[nodiscard]] static std::filesystem::path cache_path(const std::filesystem::path& asset);
    [[nodiscard]] static Subject* subject_for(const std::filesystem::path& path);

    void ensure_render_resources(const Deps& deps);
    bool serve_from_cache(const Deps& deps, const std::string& path,
                          std::filesystem::file_time_type newest);
    bool render(const Deps& deps, const DirectionalLight& light, float dt,
                Subject& subject, const std::string& path,
                std::filesystem::file_time_type newest);
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

    // Rebuilt for every render: the light, then whatever the subject adds.
    Scene scene_;
};

} // namespace fjell
