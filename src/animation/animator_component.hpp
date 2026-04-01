#pragma once

#include "animation/animation_clip.hpp"
#include "animation/animation_state_machine.hpp"
#include "animation/blend_space.hpp"
#include "animation/skeleton.hpp"
#include "scene/node_component.hpp"

#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/maths/soa_transform.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

struct Node3D;

// Per-layer animation sampling state.
struct AnimationLayer {
    const AnimationClip* clip{nullptr};
    std::string clip_path;  // asset path for event track lookup
    std::unique_ptr<ozz::animation::SamplingJob::Context> context;
    std::vector<ozz::math::SoaTransform> locals;
    float time{0.0f};
    float speed{1.0f};
    float weight{1.0f};
    bool looping{true};
};

// Per-instance animation runtime. Drives clip sampling, blending,
// and skin matrix computation for a single skinned mesh.
// Also owns the skeleton and clip data for the node it's attached to.
class AnimatorComponent : public NodeComponent {
public:
    AnimatorComponent();
    ~AnimatorComponent();
    AnimatorComponent(AnimatorComponent&&) noexcept;
    AnimatorComponent& operator=(AnimatorComponent&&) noexcept;

    // Asset paths (for serialization)
    std::vector<std::string> clip_paths;
    std::string animset_path;

    // Owned clip data (kept alive as long as the component)
    std::vector<std::shared_ptr<AnimationClip>> clips;

    // Initialize with a skeleton. Must be called before update().
    void init(const Skeleton* skeleton);

    // Set the active animation clip for single-clip playback (no state machine).
    void play(const AnimationClip* clip, bool looping = true, float speed = 1.0f);
    void play(const std::string& clip_path, const AnimationClip* clip, bool looping = true, float speed = 1.0f);

    // Load a state machine from a .fjanimset file. Clips must be resolved via set_clip().
    [[nodiscard]] Result<> load_state_machine(const std::string& path);
    // Activate state machine mode using the already-loaded state machine data.
    void enable_state_machine();
    void set_clip(const std::string& name, const AnimationClip* clip);

    // Register blend spaces for states that use them.
    void load_blend_space_1d(const std::string& path, const BlendSpace1D& bs);
    void load_blend_space_2d(const std::string& path, const BlendSpace2D& bs);

    // Load all blend spaces referenced by the state machine, and their clips.
    // base_dir is the directory containing the .fjanimset (used to resolve relative paths).
    void resolve_blend_spaces(const std::string& base_dir);

    // Set state machine parameter (forwarded to state machine).
    void set_param(const std::string& name, float value);
    [[nodiscard]] float get_param(const std::string& name) const;

    // Set playback time directly (for scrubbing).
    void set_time(float t) { layers_[0].time = t; }
    [[nodiscard]] float current_time() const { return layers_[0].time; }

    // Advance animation and compute skin matrices. Call once per frame.
    void update(float dt);

    // Output: final skin matrices (model * inverse_bind) ready for GPU upload.
    [[nodiscard]] const std::vector<glm::mat4>& skin_matrices() const { return skin_matrices_; }
    [[nodiscard]] uint32_t bone_count() const { return static_cast<uint32_t>(skin_matrices_.size()); }

    [[nodiscard]] const Skeleton* skeleton() const { return skeleton_; }
    [[nodiscard]] bool initialized() const { return skeleton_ != nullptr; }

    // Set by AnimationSystem::update() each frame for notify dispatch.
    void set_owner(Node3D* owner) { owner_ = owner; }
    [[nodiscard]] bool has_active_clip() const { return layers_[0].clip != nullptr || has_active_blend_space(); }
    [[nodiscard]] bool has_active_blend_space() const;

    // Access the state machine (for setting params, reading state, binding on_transition).
    [[nodiscard]] AnimationStateMachine& state_machine() { return state_machine_; }
    [[nodiscard]] const AnimationStateMachine& state_machine() const { return state_machine_; }

    // Access loaded blend spaces (for cloning).
    [[nodiscard]] const std::unordered_map<std::string, BlendSpace1D>& blend_spaces_1d() const { return blend_spaces_1d_; }
    [[nodiscard]] const std::unordered_map<std::string, BlendSpace2D>& blend_spaces_2d() const { return blend_spaces_2d_; }

private:
    void sample_layer(AnimationLayer& layer, float dt);
    void sample_layer_at_ratio(AnimationLayer& layer, float ratio);
    void fire_notifies(const AnimationClip* clip, float prev_time, float current_time, float duration);
    void evaluate_blend_space(const AnimationState& state, float dt);
    void compute_skin_matrices();

    const Skeleton* skeleton_{nullptr};
    Node3D* owner_{nullptr};

    // Layers: [0] = current state, [1] = next state (during crossfade)
    AnimationLayer layers_[2];

    // Blend space evaluation pool (up to 3 clips for 2D, 2 for 1D)
    std::vector<AnimationLayer> blend_layers_;

    // State machine
    AnimationStateMachine state_machine_;
    std::unordered_map<std::string, const AnimationClip*> clip_map_;
    bool use_state_machine_{false};

    // Loaded blend spaces (keyed by asset path)
    std::unordered_map<std::string, BlendSpace1D> blend_spaces_1d_;
    std::unordered_map<std::string, BlendSpace2D> blend_spaces_2d_;

    // Damped blend space parameters (smoothed toward raw state machine values)
    std::unordered_map<std::string, float> damped_params_;

    // Shared phase for phase-synced blend spaces (0.0–1.0 normalized)
    float blend_phase_{0.0f};

    // ozz output buffers
    std::vector<ozz::math::SoaTransform> blended_locals_;
    std::vector<ozz::math::Float4x4> model_matrices_;

    // Final output for GPU
    std::vector<glm::mat4> skin_matrices_;

};

} // namespace fjell
