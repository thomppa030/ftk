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

class GameUi;
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
/// bind pose, a `.fjanim` as a frame from the middle of the clip on the
/// skeleton beside it, a `.fjvfx` once its particles have had time to fill
/// in, a `.fjwidget` or `.fjui` as the game UI draws it at the project's
/// design resolution. Each thumbnail is rendered once through
/// the real pipeline, written to `.fjcache/<file>.thumb.png` next to the
/// asset, and served from that file on later runs until the asset or a file
/// it references is newer.
///
/// UI code only ever calls request(), refresh() and descriptor(); the engine
/// calls tick() once per frame in the window where offscreen previews render,
/// after the frame fence and before the main command buffer is recorded. One
/// render per frame, since each is a blocking submit and readback.
class AssetThumbnailRenderer {
public:
    static constexpr uint32_t SIZE = 128;
    /// Thumbnails kept on the GPU at once. Beyond this the ones drawn least
    /// recently are retired; they come back from the disk cache when shown
    /// again. Bounds both memory and the ImGui descriptors they hold.
    static constexpr size_t MAX_RESIDENT = 512;

    struct Deps {
        VulkanContext* vulkan_context{nullptr};
        ResourceManager* resource_manager{nullptr};
        IconCache* icon_cache{nullptr};
        ThreadPool* thread_pool{nullptr};
        const Project* project{nullptr};
        /// The editor's own game UI, whose font atlas UI thumbnails share.
        GameUi* game_ui{nullptr};
    };

    AssetThumbnailRenderer();
    ~AssetThumbnailRenderer();
    AssetThumbnailRenderer(const AssetThumbnailRenderer&) = delete;
    AssetThumbnailRenderer& operator=(const AssetThumbnailRenderer&) = delete;
    AssetThumbnailRenderer(AssetThumbnailRenderer&&) = delete;
    AssetThumbnailRenderer& operator=(AssetThumbnailRenderer&&) = delete;

    /// Whether the renderer can draw this file, judged by extension.
    [[nodiscard]] bool supports(const std::filesystem::path& path) const;

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

    /// Queue every known asset for the timestamp check. For events whose
    /// reach is unknown, such as a shader edit, which changes the look of
    /// any material using it and any mesh using such a material.
    void refresh_all();

    /// The thumbnail's ImGui descriptor, or VK_NULL_HANDLE while it is not
    /// ready. Never loads or renders; marks the thumbnail as in use so it
    /// is not the next one evicted.
    [[nodiscard]] VkDescriptorSet descriptor(const std::string& path);

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

    /// A rendered thumbnail on the CPU: row-major RGBA8, sRGB, straight
    /// alpha, SIZE × SIZE.
    struct Pixels {
        uint32_t width{0};
        uint32_t height{0};
        std::vector<uint8_t> rgba;
    };

    /// The bounding sphere a scene subject asks to have framed.
    struct Framing {
        glm::vec3 center{0.0F};
        float radius{0.5F};
    };

    /// The 3D stage scene subjects share: one small offscreen target with
    /// its own post-process resources and render graph, a private global
    /// set, and a scene rebuilt for every render around one fixed light.
    /// The camera frames whatever the subject placed.
    class SceneStage {
    public:
        /// Create the GPU resources on first use. False when the render
        /// graph could not be loaded, which disables scene thumbnails.
        bool ensure(const Deps& deps);
        /// True while in-flight command buffers may still predate the
        /// private set; a render this tick has to wait.
        [[nodiscard]] bool draining() const { return skip_frames_ > 0; }
        void advance_drain() { if (skip_frames_ > 0) --skip_frames_; }

        /// Empty the scene and place the light; the subject adds the rest.
        Scene& begin();
        /// Render the scene framed on `framing`, through the stage's own
        /// graph or `graph` when given, and read it back into `out`. A null
        /// `out` only advances what the render simulates.
        bool render(const Deps& deps, const DirectionalLight& light, float dt,
                    const Framing& framing, Pixels* out, const FjGraph* graph = nullptr);
        void shutdown(const Deps& deps);

    private:
        std::unique_ptr<OffscreenPass> target_;
        std::unique_ptr<PostProcessResources> pp_;
        const FjGraph* graph_{nullptr};
        VkDescriptorSet global_set_{VK_NULL_HANDLE};
        int skip_frames_{0};
        Scene scene_;
    };

    /// What one asset type contributes: which files it covers, which files'
    /// timestamps date its thumbnail, and the picture itself.
    class Subject {
    public:
        virtual ~Subject() = default;
        [[nodiscard]] virtual bool handles(const std::filesystem::path& path) const = 0;
        /// Newest timestamp among the asset and the files it references.
        [[nodiscard]] virtual std::filesystem::file_time_type source_time(
            const std::filesystem::path& path, const Project* project) const = 0;
        /// Produce the thumbnail. False when the asset could not be loaded.
        virtual bool render(const Deps& deps, SceneStage& stage, const DirectionalLight& light,
                            float dt, const std::string& path, Pixels& out) = 0;
        /// Release anything the subject holds on the GPU. The device is idle.
        virtual void shutdown(const Deps& /*deps*/) {}
    };

    /// A subject drawn by standing in the scene stage: it adds its nodes and
    /// says what to frame, the stage does the rest.
    class SceneSubject : public Subject {
    public:
        bool render(const Deps& deps, SceneStage& stage, const DirectionalLight& light,
                    float dt, const std::string& path, Pixels& out) final;
        /// Add the asset to `scene` and say what to frame. False when the
        /// asset could not be loaded.
        virtual bool populate(Scene& scene, const Deps& deps, const std::string& path,
                              Framing& framing) = 0;
    };

private:
    static inline AssetThumbnailRenderer* s_instance{nullptr};

    // Owned here rather than at namespace scope: a subject may hold loaded
    // asset data (a clip's ozz animation) that must go before the libraries
    // behind it tear down at exit.
    std::vector<std::unique_ptr<Subject>> subjects_;

    struct Entry {
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
        // The image behind the descriptor, owned here until retired.
        IconCache::IconEntry descriptor_entry{};
        // Newest timestamp among the asset and the files it references when
        // this entry was produced.
        std::filesystem::file_time_type source_time;
        bool failed{false};
        // tick() count when descriptor() last handed this out.
        uint64_t last_used{0};
    };

    [[nodiscard]] static std::filesystem::path cache_path(const std::filesystem::path& asset);
    [[nodiscard]] Subject* subject_for(const std::filesystem::path& path) const;

    bool serve_from_cache(const Deps& deps, const std::string& path,
                          std::filesystem::file_time_type newest);
    bool render(const Deps& deps, const DirectionalLight& light, float dt,
                Subject& subject, const std::string& path,
                std::filesystem::file_time_type newest);
    void retire(const Deps& deps, Entry& entry);
    void evict_least_recent(const Deps& deps);
    // Store a fresh upload, or fail it when ImGui could not register it.
    bool store(const Deps& deps, const std::string& path, IconCache::IconEntry entry,
               std::filesystem::file_time_type newest);

    std::deque<std::string> queue_;
    std::unordered_set<std::string> queued_;
    std::unordered_map<std::string, Entry> entries_;
    // Entries invalidated outside tick(), handed to the icon cache on the
    // next tick so they are freed at a frame boundary.
    std::vector<IconCache::IconEntry> pending_retire_;

    SceneStage stage_;
    uint64_t ticks_{0};
};

} // namespace fjell
