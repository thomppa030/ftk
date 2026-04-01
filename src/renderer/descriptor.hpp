#pragma once

#include "core/result.hpp"

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

    [[nodiscard]] static Result<DescriptorSetLayout> create(
        VkDevice device,
        std::span<const VkDescriptorSetLayoutBinding> bindings,
        const void* pNext = nullptr);

    ~DescriptorSetLayout();

    DescriptorSetLayout(const DescriptorSetLayout&) = delete;
    DescriptorSetLayout& operator=(const DescriptorSetLayout&) = delete;
    DescriptorSetLayout(DescriptorSetLayout&&) noexcept;
    DescriptorSetLayout& operator=(DescriptorSetLayout&&) noexcept;

    [[nodiscard]] VkDescriptorSetLayout handle() const { return layout_; }

private:
    DescriptorSetLayout() = default;

    VkDevice device_{VK_NULL_HANDLE};
    VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
};

class DescriptorPool {
public:
    DescriptorPool(VkDevice device,
                   std::span<const VkDescriptorPoolSize> pool_sizes,
                   uint32_t max_sets);

    [[nodiscard]] static Result<DescriptorPool> create(
        VkDevice device,
        std::span<const VkDescriptorPoolSize> pool_sizes,
        uint32_t max_sets);

    ~DescriptorPool();

    DescriptorPool(const DescriptorPool&) = delete;
    DescriptorPool& operator=(const DescriptorPool&) = delete;
    DescriptorPool(DescriptorPool&&) noexcept;
    DescriptorPool& operator=(DescriptorPool&&) noexcept;

    [[nodiscard]] VkDescriptorSet allocate(const DescriptorSetLayout& layout);
    [[nodiscard]] std::vector<VkDescriptorSet> allocate(
        const DescriptorSetLayout& layout, uint32_t count);

    [[nodiscard]] Result<VkDescriptorSet> allocate_checked(const DescriptorSetLayout& layout);

private:
    DescriptorPool() = default;

    VkDevice device_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
};

} // namespace fjell
