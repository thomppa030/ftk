#pragma once

#include "renderer/resources/model.hpp"
#include "renderer/resources/resource_handle.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace ftk {
class ThreadPool;
} // namespace ftk

namespace fjell {

class MegaBuffer;
class MeshletBuffer;
class ResourceManager;

class AssetLoader {
public:
    /// Loaded models add their geometry to `mega` and `meshlets`.
    AssetLoader(ftk::ThreadPool& pool, ResourceManager& resources, MegaBuffer& mega,
                MeshletBuffer& meshlets);

    // Request async model load. Returns handle immediately (Loading state).
    // If already loaded or in-flight, returns existing handle.
    ModelHandle load_model_async(const std::string& path);

    // Process completed loads on the main thread (GPU upload + resource emplacement).
    // Call once per frame.
    void poll();

    // Block until all pending loads complete.
    void flush();

    [[nodiscard]] uint32_t pending_count() const;

private:
    struct CompletedLoad {
        ModelHandle handle;
        FjMeshData data;
    };

    ftk::ThreadPool& pool_;
    ResourceManager& resources_;
    MegaBuffer& mega_;
    MeshletBuffer& meshlets_;

    std::mutex completed_mutex_;
    std::vector<CompletedLoad> completed_;

    mutable std::mutex in_flight_mutex_;
    uint32_t in_flight_{0};
};

} // namespace fjell
