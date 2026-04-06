#include "renderer/resources/asset_loader.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/meshlet_buffer.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "core/log.hpp"
#include "core/thread_pool.hpp"

namespace fjell {

AssetLoader::AssetLoader(ThreadPool& pool, ResourceManager& resources,
                         VmaAllocator allocator, VkDevice device,
                         VkCommandPool command_pool, VkQueue queue,
                         MegaBuffer* mega, MeshletBuffer* meshlet_buf)
    : pool_{pool}
    , resources_{resources}
    , allocator_{allocator}
    , device_{device}
    , command_pool_{command_pool}
    , queue_{queue}
    , mega_{mega}
    , meshlet_buf_{meshlet_buf} {}

ModelHandle AssetLoader::load_model_async(const std::string& path) {
    // Already loaded — return cached handle
    auto existing = resources_.find_model(path);
    if (existing.valid()) {
        return existing;
    }

    // Reserve a slot in Loading state
    auto handle = resources_.reserve_model(path);

    {
        std::lock_guard lock(in_flight_mutex_);
        ++in_flight_;
    }

    // Submit CPU work to background thread (future intentionally discarded —
    // completion is signalled via the completed_ queue)
    (void)pool_.submit([this, path, handle]() {
        try {
            // load_model() in ResourceManager resolves .fjmesh cache transparently
            auto data = Model::load_fjmesh(path);

            {
                std::lock_guard lock(completed_mutex_);
                completed_.push_back({handle, std::move(data)});
            }
        } catch (const std::exception& e) {
            FJELL_GFX_ERROR("Async model load failed for '{}': {}", path, e.what());
            {
                std::lock_guard lock(completed_mutex_);
                // Push empty data — poll() will mark as Failed
                completed_.push_back({handle, {}});
            }
        }

        {
            std::lock_guard lock(in_flight_mutex_);
            --in_flight_;
        }
    });

    return handle;
}

void AssetLoader::poll() {
    std::vector<CompletedLoad> batch;
    {
        std::lock_guard lock(completed_mutex_);
        batch.swap(completed_);
    }

    for (auto& load : batch) {
        if (load.data.vertices.empty()) {
            // Failed load
            resources_.fail_model(load.handle);
            continue;
        }

        auto model = std::make_unique<Model>(
            allocator_, device_, command_pool_, queue_,
            std::move(load.data), mega_, meshlet_buf_);

        resources_.emplace_model(load.handle, std::move(model));
    }
}

void AssetLoader::flush() {
    // Spin until all in-flight tasks complete, polling each iteration
    while (true) {
        poll();

        std::lock_guard lock(in_flight_mutex_);
        if (in_flight_ == 0) break;
    }
}

uint32_t AssetLoader::pending_count() const {
    std::lock_guard lock(in_flight_mutex_);
    return in_flight_;
}

} // namespace fjell
