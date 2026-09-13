#pragma once

#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>

namespace fjell {

inline void vk_check(VkResult result, const std::string& msg) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(msg + " (VkResult=" + std::to_string(static_cast<int>(result)) + ")");
    }
}

} // namespace fjell
