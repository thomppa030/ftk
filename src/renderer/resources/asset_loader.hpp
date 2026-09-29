#pragma once

#include "renderer/resources/model.hpp"
#include "renderer/resources/resource_handle.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <mutex>
#include <string>
#include <vector>

namespace fjell {

class MegaBuffer;
class MeshletBuffer;
class ResourceManager;
class ThreadPool;

class AssetLoader {
public:
    AssetLoader(ThreadPool& pool, ResourceManager& resources,
                VmaAllocator allocator, VkDevice device,
                gpu::Device& gpu,
                MegaBuffer* mega, MeshletBuffer* meshlet_buf);

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

    ThreadPool& pool_;
    ResourceManager& resources_;
    VmaAllocator allocator_;
    VkDevice device_;
    gpu::Device& gpu_;
    MegaBuffer* mega_;
    MeshletBuffer* meshlet_buf_;

    std::mutex completed_mutex_;
    std::vector<CompletedLoad> completed_;

    mutable std::mutex in_flight_mutex_;
    uint32_t in_flight_{0};
};

} // namespace fjell
