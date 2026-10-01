#pragma once

#include "ui/game_ui/fjfont.hpp"
#include "ui/game_ui/font_atlas.hpp"

#ifndef NDEBUG
#include "ftk/platform/file_watcher.hpp"
#endif

#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class ThreadPool;

/// Every font atlas the renderer has on the GPU, one per `.fjfont` recipe,
/// shared by every GameUi. A UI names a recipe through `@font-face`; the
/// library loads it on first use and hands the same atlas to every caller
/// after that. The engine's own font is the fallback for text that names
/// no family, or one no face declares.
///
/// A first use queues the load rather than performing it: baking a charset
/// takes long enough to stall a frame, and a texture created in the middle
/// of recording is a hazard. The bake runs on the thread pool and the atlas
/// reaches the GPU in poll(), at the start of a frame, the way models load.
/// Until then get() answers nullptr and text draws in the fallback.
///
/// Debug builds watch every loaded recipe and its font file and rebuild the
/// atlas in place when either changes, so an edit shows up in a running UI
/// without a reload. The atlas object and its bindless index stay the same
/// across a rebuild, which is what lets callers keep their pointers.
class FontLibrary {
public:
    void create(const FontAtlasGpu& gpu, const std::string& fallback_recipe);
    void destroy();

    /// Loads run on this pool once it is set; before that, and without one,
    /// they run inline in poll(), still at the frame boundary.
    void set_thread_pool(ThreadPool* pool) { pool_ = pool; }

    [[nodiscard]] const FontAtlas& fallback() const { return *fallback_; }

    enum class Status : uint8_t {
        unknown, ///< never asked for
        pending, ///< load queued or running
        ready,   ///< on the GPU
        failed,  ///< could not be loaded; logged once
    };
    [[nodiscard]] Status status(const std::string& recipe_path) const;

    /// The atlas of a recipe once it is loaded. Nullptr while the load is
    /// pending and, for good, when the recipe cannot be loaded or baked;
    /// that failure is logged once per path.
    [[nodiscard]] const FontAtlas* get(const std::string& recipe_path);

    /// Loads a recipe again from its files, on the pool, and swaps the
    /// atlas in place when done. For an editor that just wrote the recipe;
    /// debug builds also do this on their own when a watched file changes.
    void reload(const std::string& recipe_path);

    /// Bakes `recipe`, an editor's unsaved version of the recipe at
    /// `recipe_path`, on the pool and swaps it into that font's atlas in
    /// place, so everything drawing in the font shows the edit. Nothing is
    /// written. A newer preview asked for while one bakes replaces it;
    /// reload() goes back to what the file holds.
    void preview(const std::string& recipe_path, const FjFontRecipe& recipe);

    /// Bakes a recipe's atlas ahead of any use, on the pool, when it is
    /// missing or stale. Nothing reaches the GPU; a later get() reads the
    /// cache instead of baking. For a project's fonts on open and a recipe
    /// the editor just wrote.
    void prebake(const std::string& recipe_path);

    /// Once per frame, after the frame's fence: starts queued loads, takes
    /// finished ones to the GPU, and in debug builds rebuilds any atlas
    /// whose files changed.
    void poll();

private:
    // The load's outcome travels through the future behind a pointer: GCC
    // at -O3 misreads the in-place move of the Result inside std::future's
    // task setter as freeing a non-heap object and refuses the build.
    using LoadResult = std::unique_ptr<Result<FontAtlasData>>;
    using LoadFuture = std::future<LoadResult>;

    struct Entry {
        std::unique_ptr<FontAtlas> atlas;
        std::string path;
        bool queued{true};    // a load has to start
        bool failed{false};
        bool changed{false};  // a watched file changed since the last load
        // reload() was asked for by whoever wrote the recipe; the watcher's
        // events for that write are swallowed rather than reloading twice.
        bool expect_events{false};
        bool events_seen{false};
        // An unsaved recipe to bake next, from preview().
        std::optional<FjFontRecipe> preview;
        LoadFuture pending;
    };
    [[nodiscard]] static std::string key_for(const std::string& path);
    void start_load(Entry& entry);
    void finish_load(Entry& entry, Result<FontAtlasData> data);
    void watch(const std::string& key, const FontAtlas& atlas);

    FontAtlasGpu gpu_{};
    ThreadPool* pool_{nullptr};
    std::unique_ptr<FontAtlas> fallback_;
    std::unordered_map<std::string, Entry> entries_;
    // Prebakes in flight; their outcome is logged when poll() sees them done.
    std::vector<std::future<void>> prebakes_;

#ifndef NDEBUG
    platform::FileWatcher watcher_;
    bool fallback_changed_{false};
    LoadFuture fallback_pending_;
#endif
};

} // namespace fjell
