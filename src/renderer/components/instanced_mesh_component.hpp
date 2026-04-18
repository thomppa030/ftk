#pragma once

#include "renderer/frustum.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "scene/node_component.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fjell {

class Model;
class Material;

struct InstancedMeshComponent : NodeComponent {
    ModelHandle model_handle;
    MaterialHandle material_handle;
    Model* model{nullptr};
    Material* material{nullptr};
    std::string material_name;

    // Local-space instance transforms — authored / serialized.
    std::vector<glm::mat4> instances;

    // Cached world-space transforms = node_world * instances[i]. Recomputed
    // only when the node moves or `dirty` is set by editor mutations. The
    // corresponding GPU buffers are also re-uploaded at that point. Static
    // components pay zero per-frame CPU/GPU cost for their transforms.
    std::vector<glm::mat4> cached_world_transforms;
    glm::mat4 cached_node_world{0.0f}; // zero = force refresh first frame

    // Per-component GPU buffers, device-address-addressable from shaders.
    //   transforms_buffer  — current frame world transforms (CPU-uploaded on dirty)
    //   visible_buffer     — post-InstanceCullPass compacted transforms
    //   prev_buffer        — last frame's transforms (for motion vectors)
    // Buffer capacities track instances.size(); reallocated if it changes.
    std::unique_ptr<Buffer> transforms_buffer;
    std::unique_ptr<Buffer> visible_buffer;
    std::unique_ptr<Buffer> prev_buffer;
    VkDeviceAddress transforms_addr{0};
    VkDeviceAddress visible_addr{0};
    VkDeviceAddress prev_addr{0};
    uint32_t gpu_capacity{0}; // number of mat4s each buffer can hold

    // Union of every instance's model-local-AABB transformed by its local-space
    // instance matrix, expressed in the component's local (node-parent) space.
    // Cached across frames; recomputed when `dirty` is set or the model changes.
    AABB local_bounds{};
    bool dirty{true};
};

} // namespace fjell
