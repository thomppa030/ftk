#pragma once

#include "renderer/draw_data_descriptor.hpp"
#include "renderer/frustum.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/render_queue.hpp"
#include "scene/component_type.hpp"
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

class PipelineCache;

class AnimationSystem;
class GpuContext;
class Scene;
struct Node;
class ShadowPass;
class ThreadPool;
struct RenderStats;

/// Input parameters for SceneGatherer::gather().
struct GatherInput {
    Scene* scene{nullptr};
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 camera_position{0.0f};
    uint32_t frame_index{0};
    const ShadowPass* shadow_pass{nullptr};
    ThreadPool* pool{nullptr};
    RenderStats* stats{nullptr};
    AnimationSystem* animation_system{nullptr};
    /// The material pipelines, whose cull modes the camera batch notes per
    /// draw for the meshlet cull.
    const PipelineCache* pipelines{nullptr};
};

/// Extracts renderable data from the scene: frustum culling, draw command
/// building, and indirect batch construction.
class SceneGatherer {
public:
    void init(GpuContext& gpu);

    /// Frustum-cull the scene, build render queue and indirect batches.
    void gather(const GatherInput& input);

    /// Every surface of `scene` a ray could hit within `reach`, whatever any
    /// camera sees: what the acceleration structure is built over. Static
    /// meshes give one command per material slot and instance providers
    /// their draws, transparent ones included; skinned meshes have no BLAS
    /// and are left out. Read back through rt_commands().
    void gather_rt(Scene& scene, const AABB& reach);
    [[nodiscard]] const std::vector<DrawCommand>& rt_commands() const { return rt_commands_; }

    [[nodiscard]] const RenderQueue& render_queue() const { return render_queue_; }
    [[nodiscard]] RenderQueue& render_queue() { return render_queue_; }
    [[nodiscard]] IndirectBatch* indirect_batch() { return indirect_batch_.get(); }
    [[nodiscard]] IndirectBatch* shadow_batch(uint32_t cascade) { return shadow_batches_[cascade].get(); }
    [[nodiscard]] const std::vector<DrawCommand>& shadow_cmds(uint32_t cascade) const {
        return shadow_commands_[cascade];
    }
    [[nodiscard]] DrawDataDescriptor& draw_data_descriptor() { return draw_data_descriptor_; }
    [[nodiscard]] DrawDataDescriptor& shadow_draw_data(uint32_t cascade) { return shadow_draw_data_[cascade]; }

    [[nodiscard]] const glm::vec3& camera_position() const { return camera_position_; }
    [[nodiscard]] const glm::mat4& view_matrix() const { return view_matrix_; }
    [[nodiscard]] const glm::mat4& projection_matrix() const { return projection_matrix_; }

    void set_camera_state(const glm::vec3& pos, const glm::mat4& view,
                          const glm::mat4& projection) {
        camera_position_ = pos;
        view_matrix_ = view;
        projection_matrix_ = projection;
    }

private:
    RenderQueue render_queue_;
    glm::vec3 camera_position_{0.0f};
    glm::mat4 view_matrix_{1.0f};
    glm::mat4 projection_matrix_{1.0f};

    std::unique_ptr<IndirectBatch> indirect_batch_;
    DrawDataDescriptor draw_data_descriptor_;

    // Per-cascade shadow batches (each cascade only draws its own geometry)
    std::array<std::unique_ptr<IndirectBatch>, FrameContext::MAX_SHADOW_CASCADES> shadow_batches_;
    std::array<DrawDataDescriptor, FrameContext::MAX_SHADOW_CASCADES> shadow_draw_data_;

    // Reusable per-frame scratch (avoid heap allocs every frame)
    std::array<std::vector<DrawCommand>, FrameContext::MAX_SHADOW_CASCADES> shadow_commands_;
    std::vector<std::vector<DrawCommand>> local_commands_;
    // Per-chunk, per-cascade shadow command scratch
    std::vector<std::array<std::vector<DrawCommand>, FrameContext::MAX_SHADOW_CASCADES>> local_shadow_commands_;
    // Per-chunk scratch for de-duplicating a mesh's material slots
    std::vector<std::vector<uint32_t>> local_slot_scratch_;
    // Per-gather scratch: the drawable component types, and the nodes that
    // carry one.
    std::vector<ComponentTypeId> drawable_types_;
    std::vector<Node*> candidates_;
    // What gather_rt() found, and its slot de-duplication scratch.
    std::vector<DrawCommand> rt_commands_;
    std::vector<uint32_t> rt_slot_scratch_;

    // The nodes something could draw, into candidates_.
    void collect_candidates(Scene& scene);
    std::vector<uint32_t> local_total_;
    std::vector<uint32_t> local_culled_;
    std::vector<DrawCommand> empty_transparent_;
};

} // namespace fjell
