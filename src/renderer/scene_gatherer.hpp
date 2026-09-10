#pragma once

#include "renderer/gpu/draw_data_descriptor.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/render_queue.hpp"
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

class AnimationSystem;
class GpuContext;
class Scene;
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
};

/// Extracts renderable data from the scene: frustum culling, draw command
/// building, and indirect batch construction.
class SceneGatherer {
public:
    void init(GpuContext& gpu);

    /// Frustum-cull the scene, build render queue and indirect batches.
    void gather(const GatherInput& input);

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
    [[nodiscard]] const glm::mat4& prev_view_proj() const { return prev_view_proj_; }
    [[nodiscard]] const glm::mat4& prev_view() const { return prev_view_; }

    void set_camera_state(const glm::vec3& pos, const glm::mat4& view,
                          const glm::mat4& projection) {
        camera_position_ = pos;
        view_matrix_ = view;
        projection_matrix_ = projection;
    }

    void update_prev_view_proj() {
        prev_view_proj_ = projection_matrix_ * view_matrix_;
        prev_view_ = view_matrix_;
    }

private:
    RenderQueue render_queue_;
    glm::vec3 camera_position_{0.0f};
    glm::mat4 view_matrix_{1.0f};
    glm::mat4 projection_matrix_{1.0f};
    glm::mat4 prev_view_proj_{1.0f};
    glm::mat4 prev_view_{1.0f};

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
    std::vector<uint32_t> local_total_;
    std::vector<uint32_t> local_culled_;
    std::vector<DrawCommand> empty_transparent_;
};

} // namespace fjell
