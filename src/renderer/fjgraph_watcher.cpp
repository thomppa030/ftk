#ifndef NDEBUG

#include "renderer/fjgraph_watcher.hpp"
#include "core/log.hpp"

namespace fjell {

FjGraphWatcher::FjGraphWatcher(std::string graph_path, ReloadCallback callback)
    : graph_path_{std::move(graph_path)}
    , callback_{std::move(callback)} {
    watcher_.watch(graph_path_, [this](const std::filesystem::path&) {
        changed_ = true;
    });
    FJELL_GFX_DEBUG("FjGraphWatcher: watching {}", graph_path_);
}

bool FjGraphWatcher::poll() {
    changed_ = false;
    (void)watcher_.poll();

    if (!changed_ || !callback_) { return false; }

    FJELL_CORE_INFO("FjGraphWatcher: {} changed, reloading", graph_path_);
    callback_(graph_path_);
    return true;
}

} // namespace fjell

#endif // NDEBUG
