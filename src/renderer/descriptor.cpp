#include "renderer/descriptor.hpp"

#include <stdexcept>

namespace fjell {

DescriptorSetLayout::DescriptorSetLayout(
    VkDevice device,
    std::span<const VkDescriptorSetLayoutBinding> bindings)
    : device_{device} {
    VkDescriptorSetLayoutCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    create_info.bindingCount = static_cast<uint32_t>(bindings.size());
    create_info.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(device_, &create_info, nullptr, &layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor set layout");
    }
}

DescriptorSetLayout::~DescriptorSetLayout() {
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
    }
}

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

DescriptorPool::~DescriptorPool() {
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, pool_, nullptr);
    }
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

} // namespace fjell
