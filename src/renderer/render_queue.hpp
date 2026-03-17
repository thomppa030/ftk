#pragma once

#include "renderer/uniform_types.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace fjell {

class Model;
struct RenderStats;

struct DrawCommand {
    uint64_t sort_key{0};
    glm::mat4 world_matrix{1.0f};
    PbrPushConstants push{};
    VkDescriptorSet material_set{VK_NULL_HANDLE};
    Model* model{nullptr};
};

class RenderQueue {
public:
    void clear();
    void push(const DrawCommand& cmd);
    void sort();
    void execute(VkCommandBuffer cmd, VkPipelineLayout layout,
                 RenderStats* stats = nullptr) const;

    [[nodiscard]] bool empty() const { return commands_.empty(); }
    [[nodiscard]] const std::vector<DrawCommand>& commands() const { return commands_; }

    // Build a sort key: material first (minimize descriptor rebinds), then mesh
    static uint64_t make_sort_key(uint32_t material_id, uint32_t mesh_id);

private:
    std::vector<DrawCommand> commands_;
};

} // namespace fjell
