#pragma once

#include "animation/animation_clip.hpp"
#include "animation/animation_events.hpp"
#include "animation/animation_state_machine.hpp"
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

// Per-layer animation sampling state.
struct AnimationLayer {
    const AnimationClip* clip{nullptr};
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

    // Load a state machine from a .fjanimset file. Clips must be resolved via set_clip().
    void load_state_machine(const std::string& path);
    void set_clip(const std::string& name, const AnimationClip* clip);

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
    [[nodiscard]] bool has_active_clip() const { return layers_[0].clip != nullptr; }

    // Per-clip event tracks. Keyed by clip path.
    void set_event_track(const std::string& clip_path, AnimationEventTrack track);
    [[nodiscard]] AnimationEventTrack* event_track(const std::string& clip_path);

    // Access the state machine (for setting params, reading state, binding on_transition).
    [[nodiscard]] AnimationStateMachine& state_machine() { return state_machine_; }
    [[nodiscard]] const AnimationStateMachine& state_machine() const { return state_machine_; }

private:
    void sample_layer(AnimationLayer& layer, float dt);
    void compute_skin_matrices();

    const Skeleton* skeleton_{nullptr};

    // Layers: [0] = current state, [1] = next state (during crossfade)
    AnimationLayer layers_[2];

    // State machine
    AnimationStateMachine state_machine_;
    std::unordered_map<std::string, const AnimationClip*> clip_map_;
    bool use_state_machine_{false};

    // ozz output buffers
    std::vector<ozz::math::SoaTransform> blended_locals_;
    std::vector<ozz::math::Float4x4> model_matrices_;

    // Final output for GPU
    std::vector<glm::mat4> skin_matrices_;

    // Event tracks per clip path
    std::unordered_map<std::string, AnimationEventTrack> event_tracks_;
};

} // namespace fjell
