#pragma once

#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/readback.hpp"
#include "ftk/gpu/texture.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/viewport_source.hpp"
#include "scene/scene.hpp"

#include <imgui.h>


#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fjell {

class GameUi;
class IconCache;
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
/// design resolution, a `.fjfont` as a sample line in that font, a `.fjell`
/// as it looked when it was last saved. Each
/// thumbnail is rendered once through the real pipeline, written to
/// `.fjcache/<file>.thumb.png` next to the asset, and served from that file
/// on later runs until the asset or a file it references is newer.
///
/// UI code only ever calls request(), refresh() and texture(); the engine
/// calls tick() once per frame in the window where offscreen previews render,
/// after the frame fence and before the frame's views are recorded. A render
/// records into the frame with its readback; the picture comes back once
/// that frame has finished, and is fitted and written to its cache file on
/// the thread pool. One render a tick.
class AssetThumbnailRenderer {
public:
    static constexpr uint32_t SIZE = 128;
    /// Thumbnails kept on the GPU at once. Beyond this the ones drawn least
    /// recently are let go; they come back from the disk cache when shown
    /// again. Bounds both memory and the ImGui images they hold.
    static constexpr size_t MAX_RESIDENT = 512;

    struct Deps {
        VulkanContext* vulkan_context{nullptr};
        ResourceManager* resource_manager{nullptr};
        IconCache* icon_cache{nullptr};
        ThreadPool* thread_pool{nullptr};
        const Project* project{nullptr};
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
    /// every frame texture() comes back empty; an asset that failed to
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

    /// The thumbnail as ImGui draws it, or none while it is not ready. Never
    /// loads or renders; marks the thumbnail as in use so it is not the next
    /// one evicted.
    [[nodiscard]] ImTextureID texture(const std::string& path);

    /// Forget a thumbnail and delete its cache file. The frames still
    /// drawing it keep it until they are done.
    void invalidate(const std::string& path);

    /// A camera to shoot a scene with.
    struct View {
        glm::mat4 view{1.0F};
        glm::mat4 projection{1.0F};
        glm::vec3 position{0.0F};
    };

    /// Take a scene's thumbnail from a scene that is already loaded, as
    /// seen from `view`: the scene editor calls this when it saves, since
    /// rendering a scene on demand would mean loading everything in it.
    /// The render happens on the next tick, so `scene` has to outlive it.
    void capture_scene(Scene& scene, const std::string& fjell_path, const View& view);

    /// Whether a thumbnail for this file can be rendered on demand. A scene's
    /// cannot: it exists only from a save. Regenerating a folder skips these
    /// rather than throwing away a thumbnail nothing can bring back.
    [[nodiscard]] bool regenerates(const std::filesystem::path& path) const;

    /// Serve or render at most one queued thumbnail. Call once per frame in
    /// the offscreen preview window.
    void tick(const Deps& deps, float dt);

    /// Release GPU resources. Call after the device is idle.
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

    /// A render's picture on its way back from the frame that drew it: the
    /// read, and whether it is to be fitted and centred in the SIZE square
    /// (a crop of a UI) or is the thumbnail as read (the scene stage's).
    struct Capture {
        gpu::Readback read;
        bool centre{false};
    };

    /// The bounding sphere a scene subject asks to have framed.
    struct Framing {
        glm::vec3 center{0.0F};
        float radius{0.5F};
    };

    /// How one render through the scene stage differs from the default,
    /// which is what an asset thumbnail wants: the stage's own graph, the
    /// editor's backdrop, no shadows, and a fixed exposure.
    struct RenderOptions {
        const FjGraph* graph{nullptr};
        const SkyBackdrop* backdrop{&SkyBackdrop::editor_preview()};
        bool skip_shadows{true};
        // Exposure is one adapted value shared by every viewport. Pinned,
        // a thumbnail does not depend on how bright the open scene is;
        // left alone, a capture of that very scene matches its viewport.
        bool pin_exposure{true};
    };

    /// The 3D stage scene subjects share: one small offscreen target with
    /// its own post-process resources and render graph, and a scene rebuilt
    /// for every render around one fixed light. The camera frames whatever
    /// the subject placed.
    class SceneStage {
    public:
        /// Create the GPU resources on first use. False when the render
        /// graph could not be loaded, which disables scene thumbnails.
        bool ensure(const Deps& deps);

        /// Empty the scene and place the light; the subject adds the rest.
        Scene& begin();
        /// Render the stage's scene framed on `framing`, through the stage's
        /// own graph or `graph` when given, into the frame.
        bool render(const Deps& deps, float dt,
                    const Framing& framing, const FjGraph* graph = nullptr);
        /// Render any scene from an explicit camera into the stage's target,
        /// in the frame.
        bool render_view(const Deps& deps, float dt,
                         Scene& scene, const View& view, const RenderOptions& options);
        /// Read back what the last render drew, from the frame after it.
        [[nodiscard]] std::optional<Capture> read_back(const Deps& deps);
        void shutdown(const Deps& deps);

    private:
        std::unique_ptr<OffscreenPass> target_;
        std::unique_ptr<PostProcessResources> pp_;
        const FjGraph* graph_{nullptr};
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
        /// Render the thumbnail into the frame and read it back. None when the
        /// asset could not be loaded.
        virtual std::optional<Capture> render(const Deps& deps, SceneStage& stage, float dt,
                                              const std::string& path) = 0;
        /// Whether render() can produce the thumbnail from the file alone.
        [[nodiscard]] virtual bool renders_on_demand() const { return true; }
        /// False while something render() needs is still on its way — a
        /// font atlas being baked, say. The path is tried again on a later
        /// tick rather than recorded as a failure.
        [[nodiscard]] virtual bool ready(const Deps& /*deps*/, const std::string& /*path*/) {
            return true;
        }
        /// Release anything the subject holds on the GPU. The device is idle.
        virtual void shutdown(const Deps& /*deps*/) {}
    };

    /// A subject drawn by standing in the scene stage: it adds its nodes and
    /// says what to frame, the stage does the rest.
    class SceneSubject : public Subject {
    public:
        std::optional<Capture> render(const Deps& deps, SceneStage& stage, float dt,
                                      const std::string& path) final;
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
        // The thumbnail; none for one that failed.
        gpu::Owned<gpu::Texture> texture;
        // Newest timestamp among the asset and the files it references when
        // this entry was produced.
        std::filesystem::file_time_type source_time;
        bool failed{false};
        // tick() count when texture() last handed this out.
        uint64_t last_used{0};
    };

    [[nodiscard]] static std::filesystem::path cache_path(const std::filesystem::path& asset);
    [[nodiscard]] Subject* subject_for(const std::filesystem::path& path) const;

    bool serve_from_cache(const Deps& deps, const std::string& path,
                          std::filesystem::file_time_type newest);
    bool render(const Deps& deps, float dt,
                Subject& subject, const std::string& path,
                std::filesystem::file_time_type newest);
    // Sends `capture` on its way as `path`'s thumbnail.
    void track(const std::string& path, std::filesystem::file_time_type newest, Capture capture);
    // Hands the reads whose frames have finished to the pool, which fits
    // them and writes their cache files.
    void finish_reads(const Deps& deps);
    // Uploads what the pool has finished as the assets' thumbnails.
    void store_finished(const Deps& deps);
    void render_pending_capture(const Deps& deps, float dt);
    void evict_least_recent();
    // Store a fresh upload, or fail when there is none.
    bool store(const std::string& path, gpu::Owned<gpu::Texture> texture,
               std::filesystem::file_time_type newest);

    std::deque<std::string> queue_;
    std::unordered_set<std::string> queued_;
    std::unordered_map<std::string, Entry> entries_;

    // Renders on their way back: their reads until the frames that drew
    // them finish, then on the pool. Each path's latest render is the one
    // stored; an invalidated path's is dropped.
    struct InFlight {
        std::string path;
        std::filesystem::file_time_type newest;
        uint64_t render{0};
        Capture capture;
    };
    std::vector<InFlight> in_flight_;
    std::unordered_map<std::string, uint64_t> rendering_;
    uint64_t renders_{0};
    // What the pool has finished, shared with its jobs, which may finish
    // after the renderer is gone.
    struct Inbox;
    std::shared_ptr<Inbox> inbox_;

    SceneStage stage_;
    uint64_t ticks_{0};

    // A scene capture asked for outside the preview window, taken on the
    // next tick. A later request replaces an earlier one.
    struct PendingCapture {
        Scene* scene{nullptr};
        std::string path;
        View view;
    };
    std::optional<PendingCapture> pending_capture_;
};

} // namespace fjell
