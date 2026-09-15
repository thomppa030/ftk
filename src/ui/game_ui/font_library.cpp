#include "ui/game_ui/font_library.hpp"
#include "ui/game_ui/font_atlas_build.hpp"
#include "core/log.hpp"
#include "core/thread_pool.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace fjell {

namespace {

template <typename T>
bool ready(const std::future<T>& future) {
    return future.valid() &&
           future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// The whole prebake, run wherever the pool puts it; it reports for itself.
void prebake_now(const std::string& path) {
    auto baked = ensure_font_atlas(path);
    if (!baked) {
        FJELL_CORE_ERROR("FontLibrary: {}", baked.error());
    } else if (*baked) {
        FJELL_GFX_INFO("Font atlas baked ahead of use: {}", path);
    }
}

} // namespace

std::string FontLibrary::key_for(const std::string& path) {
    return std::filesystem::path(path).lexically_normal().string();
}

void FontLibrary::create(const FontAtlasGpu& gpu, const std::string& fallback_recipe) {
    gpu_ = gpu;
    fallback_ = std::make_unique<FontAtlas>();
    // Startup, before any frame: the one load that may block.
    auto data = FontAtlas::load_data(fallback_recipe);
    if (data) {
        fallback_->create(fallback_recipe, gpu_, std::move(*data));
        watch({}, *fallback_);
    } else {
        FJELL_CORE_ERROR("FontLibrary: the default font {} did not load; text will not draw",
                         fallback_recipe);
    }
}

void FontLibrary::destroy() {
#ifndef NDEBUG
    watcher_.clear();
    if (fallback_pending_.valid()) fallback_pending_.wait();
#endif
    for (auto& prebake : prebakes_) {
        if (prebake.valid()) prebake.wait();
    }
    prebakes_.clear();
    for (auto& [key, entry] : entries_) {
        if (entry.pending.valid()) entry.pending.wait();
    }
    entries_.clear();
    fallback_.reset();
}

const FontAtlas* FontLibrary::get(const std::string& recipe_path) {
    auto key = key_for(recipe_path);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        Entry entry;
        entry.path = key;
        it = entries_.emplace(key, std::move(entry)).first;
    }
    const auto& entry = it->second;
    if (entry.failed || !entry.atlas) return nullptr;
    return entry.atlas.get();
}

void FontLibrary::prebake(const std::string& recipe_path) {
    auto key = key_for(recipe_path);
    if (pool_) {
        prebakes_.push_back(pool_->submit([key] { prebake_now(key); }));
    } else {
        prebake_now(key);
    }
}

void FontLibrary::start_load(Entry& entry) {
    entry.queued = false;
    auto path = entry.path;
    if (pool_) {
        entry.pending = pool_->submit([path] { return FontAtlas::load_data(path); });
    } else {
        finish_load(entry, FontAtlas::load_data(path));
    }
}

void FontLibrary::finish_load(Entry& entry, Result<FontAtlasData> data) {
    if (!data) {
        if (!entry.atlas) {
            // The load already logged why; this is the one line per path
            // that says what happens next.
            FJELL_CORE_WARN("FontLibrary: {} is unavailable, text in it uses the default font",
                            entry.path);
            entry.failed = true;
        }
        // A reload that failed keeps the atlas that was there.
        return;
    }
    if (entry.atlas) {
        entry.atlas->reload(std::move(*data));
        return;
    }
    entry.atlas = std::make_unique<FontAtlas>();
    entry.atlas->create(entry.path, gpu_, std::move(*data));
    entry.failed = false;
    watch(entry.path, *entry.atlas);
}

void FontLibrary::watch(const std::string& key, const FontAtlas& atlas) {
#ifndef NDEBUG
    // Both files that go into the bake; a change to either is a rebuild.
    for (const auto& path : {atlas.recipe_path(), atlas.source_path()}) {
        if (path.empty()) continue;
        bool ok = watcher_.watch(path, [this, key](const std::filesystem::path&) {
            if (key.empty()) {
                fallback_changed_ = true;
            } else if (auto it = entries_.find(key); it != entries_.end()) {
                it->second.changed = true;
            }
        });
        if (!ok) FJELL_GFX_DEBUG("FontLibrary: cannot watch {}", path);
    }
#else
    (void)key;
    (void)atlas;
#endif
}

void FontLibrary::poll() {
    std::erase_if(prebakes_, [](std::future<void>& f) { return ready(f); });

    for (auto& [key, entry] : entries_) {
        if (entry.pending.valid()) {
            if (!ready(entry.pending)) continue;
            finish_load(entry, entry.pending.get());
            entry.pending = {};
        }
        if (entry.queued) start_load(entry);
    }

#ifndef NDEBUG
    (void)watcher_.poll();
    for (auto& [key, entry] : entries_) {
        // One rebuild in flight at a time; a change during it queues the next.
        if (entry.changed && entry.atlas && !entry.pending.valid()) {
            entry.changed = false;
            entry.queued = true;
            start_load(entry);
        }
    }
    if (fallback_pending_.valid() && ready(fallback_pending_)) {
        auto data = fallback_pending_.get();
        fallback_pending_ = {};
        if (data) fallback_->reload(std::move(*data));
    }
    if (fallback_changed_ && fallback_->valid() && !fallback_pending_.valid()) {
        fallback_changed_ = false;
        auto path = fallback_->recipe_path();
        if (pool_) {
            fallback_pending_ = pool_->submit([path] { return FontAtlas::load_data(path); });
        } else if (auto data = FontAtlas::load_data(path)) {
            fallback_->reload(std::move(*data));
        }
    }
#endif
}

} // namespace fjell
