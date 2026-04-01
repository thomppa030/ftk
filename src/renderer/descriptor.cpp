#include "renderer/descriptor.hpp"

#include <stdexcept>
#include <utility>

namespace fjell {

// ── DescriptorSetLayout ────────────────────────────────────────────────

DescriptorSetLayout::DescriptorSetLayout(
    VkDevice device,
    std::span<const VkDescriptorSetLayoutBinding> bindings,
    const void* pNext)
    : device_{device} {
    VkDescriptorSetLayoutCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    create_info.pNext = pNext;
    create_info.bindingCount = static_cast<uint32_t>(bindings.size());
    create_info.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(device_, &create_info, nullptr, &layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor set layout");
    }
}

Result<DescriptorSetLayout> DescriptorSetLayout::create(
    VkDevice device,
    std::span<const VkDescriptorSetLayoutBinding> bindings,
    const void* pNext) {
    DescriptorSetLayout dsl{};
    dsl.device_ = device;

    VkDescriptorSetLayoutCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    create_info.pNext = pNext;
    create_info.bindingCount = static_cast<uint32_t>(bindings.size());
    create_info.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(device, &create_info, nullptr, &dsl.layout_) != VK_SUCCESS) {
        return make_error("Failed to create descriptor set layout");
    }
    return dsl;
}

DescriptorSetLayout::~DescriptorSetLayout() {
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
    }
}

DescriptorSetLayout::DescriptorSetLayout(DescriptorSetLayout&& other) noexcept
    : device_{other.device_}
    , layout_{std::exchange(other.layout_, VK_NULL_HANDLE)} {
}

DescriptorSetLayout& DescriptorSetLayout::operator=(DescriptorSetLayout&& other) noexcept {
    if (this != &other) {
        if (layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
        device_ = other.device_;
        layout_ = std::exchange(other.layout_, VK_NULL_HANDLE);
    }
    return *this;
}

// ── DescriptorPool ─────────────────────────────────────────────────────

DescriptorPool::DescriptorPool(VkDevice device,
                               std::span<const VkDescriptorPoolSize> pool_sizes,
                               uint32_t max_sets)
    : device_{device} {
    VkDescriptorPoolCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    create_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    create_info.pPoolSizes = pool_sizes.data();
    create_info.maxSets = max_sets;

    if (vkCreateDescriptorPool(device_, &create_info, nullptr, &pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor pool");
    }
}

Result<DescriptorPool> DescriptorPool::create(
    VkDevice device,
    std::span<const VkDescriptorPoolSize> pool_sizes,
    uint32_t max_sets) {
    DescriptorPool dp{};
    dp.device_ = device;

    VkDescriptorPoolCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    create_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    create_info.pPoolSizes = pool_sizes.data();
    create_info.maxSets = max_sets;

    if (vkCreateDescriptorPool(device, &create_info, nullptr, &dp.pool_) != VK_SUCCESS) {
        return make_error("Failed to create descriptor pool");
    }
    return dp;
}

DescriptorPool::~DescriptorPool() {
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, pool_, nullptr);
    }
}

DescriptorPool::DescriptorPool(DescriptorPool&& other) noexcept
    : device_{other.device_}
    , pool_{std::exchange(other.pool_, VK_NULL_HANDLE)} {
}

DescriptorPool& DescriptorPool::operator=(DescriptorPool&& other) noexcept {
    if (this != &other) {
        if (pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, pool_, nullptr);
        device_ = other.device_;
        pool_ = std::exchange(other.pool_, VK_NULL_HANDLE);
    }
    return *this;
}

VkDescriptorSet DescriptorPool::allocate(const DescriptorSetLayout& layout) {
    return allocate(layout, 1)[0];
}

std::vector<VkDescriptorSet> DescriptorPool::allocate(
    const DescriptorSetLayout& layout, uint32_t count) {
    std::vector<VkDescriptorSetLayout> layouts(count, layout.handle());

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = pool_;
    alloc_info.descriptorSetCount = count;
    alloc_info.pSetLayouts = layouts.data();

    std::vector<VkDescriptorSet> sets(count);
    if (vkAllocateDescriptorSets(device_, &alloc_info, sets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate descriptor sets");
    }
    return sets;
}

Result<VkDescriptorSet> DescriptorPool::allocate_checked(const DescriptorSetLayout& layout) {
    VkDescriptorSetLayout vk_layout = layout.handle();

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &vk_layout;

    VkDescriptorSet set{VK_NULL_HANDLE};
    if (vkAllocateDescriptorSets(device_, &alloc_info, &set) != VK_SUCCESS) {
        return make_error("Failed to allocate descriptor set");
    }
    return set;
}

} // namespace fjell
