#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <unordered_map>

namespace fjell {

class IconCache {
public:
    IconCache(VkDevice device, VkPhysicalDevice physical_device,
              VkCommandPool command_pool, VkQueue queue,
              const std::string& icons_dir);
    ~IconCache();

    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;
    IconCache(IconCache&&) = delete;
    IconCache& operator=(IconCache&&) = delete;

    /// Returns the ImGui descriptor set for a named icon (e.g. "folder", "mesh").
    /// Returns VK_NULL_HANDLE if not found.
    [[nodiscard]] VkDescriptorSet icon(const std::string& name) const;

    [[nodiscard]] size_t count() const { return icons_.size(); }

private:
    struct IconEntry {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VkSampler sampler{VK_NULL_HANDLE};
        VkDescriptorSet descriptor{VK_NULL_HANDLE};
    };

    void load_icon(const std::string& name, const std::string& path);

    VkDevice device_;
    VkPhysicalDevice physical_device_;
    VkCommandPool command_pool_;
    VkQueue queue_;

    std::unordered_map<std::string, IconEntry> icons_;
};

} // namespace fjell
