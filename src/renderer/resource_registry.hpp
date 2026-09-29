#pragma once

#include <gpu/buffer.hpp>
#include "gpu/texture.hpp"

#include <vulkan/vulkan.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace fjell {

/// Per-frame registry mapping named resources to Vulkan handles and
/// FrameGraph image IDs. Bridges the string-based dependency system
/// (RenderPipeline) with the actual image tracking (FrameGraph).
class ResourceRegistry {
public:
    struct ImageResource {
        uint32_t frame_graph_id{0};
        /// The texture as the GPU interface knows it.
        gpu::Texture texture{};
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VkSampler sampler{VK_NULL_HANDLE};
        uint32_t mip_count{0};
    };

    struct BufferResource {
        gpu::Buffer buffer{};
        /// The bytes in use, from its start.
        uint64_t size{0};
    };

    // ── Registration (called by frame setup or producer passes) ────────

    void register_image(const std::string& name, const ImageResource& resource) {
        images_[name] = resource;
    }

    void register_buffer(const std::string& name, const BufferResource& resource) {
        buffers_[name] = resource;
    }

    // ── Lookup (called by consumer passes) ─────────────────────────────

    /// Find an image resource by name. Returns nullptr if not registered.
    [[nodiscard]] const ImageResource* find_image(const std::string& name) const {
        auto it = images_.find(name);
        if (it != images_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    /// Find a buffer resource by name. Returns nullptr if not registered.
    [[nodiscard]] const BufferResource* find_buffer(const std::string& name) const {
        auto it = buffers_.find(name);
        if (it != buffers_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    /// Get the FrameGraph image ID for a named resource.
    /// Asserts if the resource is not registered — use find_image() for
    /// optional resources.
    [[nodiscard]] uint32_t image_id(const std::string& name) const {
        const auto* res = find_image(name);
        assert(res != nullptr && "ResourceRegistry::image_id: resource not found");
        return res->frame_graph_id;
    }

    /// The texture registered under `name`; invalid if none is.
    [[nodiscard]] gpu::Texture texture(const std::string& name) const {
        const auto* res = find_image(name);
        return res != nullptr ? res->texture : gpu::Texture{};
    }

    /// Get the VkImageView for a named resource.
    /// Returns VK_NULL_HANDLE if not registered.
    [[nodiscard]] VkImageView image_view(const std::string& name) const {
        const auto* res = find_image(name);
        return res != nullptr ? res->view : VK_NULL_HANDLE;
    }

    /// Get the VkSampler for a named resource.
    /// Returns VK_NULL_HANDLE if not registered.
    [[nodiscard]] VkSampler image_sampler(const std::string& name) const {
        const auto* res = find_image(name);
        return res != nullptr ? res->sampler : VK_NULL_HANDLE;
    }

    /// Clear all entries. Called at the start of each frame.
    void clear() {
        images_.clear();
        buffers_.clear();
    }

private:
    std::unordered_map<std::string, ImageResource> images_;
    std::unordered_map<std::string, BufferResource> buffers_;
};

} // namespace fjell
