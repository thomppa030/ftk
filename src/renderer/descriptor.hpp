#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <vector>

namespace fjell {

class DescriptorSetLayout {
public:
    DescriptorSetLayout(VkDevice device,
                        std::span<const VkDescriptorSetLayoutBinding> bindings,
                        const void* pNext = nullptr);
    ~DescriptorSetLayout();

    DescriptorSetLayout(const DescriptorSetLayout&) = delete;
    DescriptorSetLayout& operator=(const DescriptorSetLayout&) = delete;
    DescriptorSetLayout(DescriptorSetLayout&&) = delete;
    DescriptorSetLayout& operator=(DescriptorSetLayout&&) = delete;

    [[nodiscard]] VkDescriptorSetLayout handle() const { return layout_; }

private:
    VkDevice device_;
    VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
};

class DescriptorPool {
public:
    DescriptorPool(VkDevice device,
                   std::span<const VkDescriptorPoolSize> pool_sizes,
                   uint32_t max_sets);
    ~DescriptorPool();

    DescriptorPool(const DescriptorPool&) = delete;
    DescriptorPool& operator=(const DescriptorPool&) = delete;
    DescriptorPool(DescriptorPool&&) = delete;
    DescriptorPool& operator=(DescriptorPool&&) = delete;

    [[nodiscard]] VkDescriptorSet allocate(const DescriptorSetLayout& layout);
    [[nodiscard]] std::vector<VkDescriptorSet> allocate(
        const DescriptorSetLayout& layout, uint32_t count);

private:
    VkDevice device_;
    VkDescriptorPool pool_{VK_NULL_HANDLE};
};

} // namespace fjell
