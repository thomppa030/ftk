#include "animation/animator_component.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"

#include <ozz/base/maths/simd_math.h>

#include <cstring>

namespace fjell {

AnimatorComponent::AnimatorComponent() = default;
AnimatorComponent::~AnimatorComponent() = default;
AnimatorComponent::AnimatorComponent(AnimatorComponent&&) noexcept = default;
AnimatorComponent& AnimatorComponent::operator=(AnimatorComponent&&) noexcept = default;

void AnimatorComponent::init(const Skeleton* skeleton) {
    skeleton_ = skeleton;
    if (!skeleton_) return;

    int num_soa = skeleton_->num_soa_joints();
    int num_joints = skeleton_->num_joints();

    // Allocate sampling contexts and local transform buffers for both layers
    for (auto& layer : layers_) {
        layer.context = std::make_unique<ozz::animation::SamplingJob::Context>(num_joints);
        layer.locals.resize(num_soa);
        layer.clip = nullptr;
        layer.time = 0.0f;
        layer.weight = 0.0f;
    }

    blended_locals_.resize(num_soa);
    model_matrices_.resize(num_joints);
    skin_matrices_.resize(num_joints);
}

void AnimatorComponent::play(const AnimationClip* clip, bool looping, float speed) {
    use_state_machine_ = false;

    layers_[0].clip = clip;
    layers_[0].time = 0.0f;
    layers_[0].speed = speed;
    layers_[0].weight = 1.0f;
    layers_[0].looping = looping;
    layers_[0].context->Invalidate();

    layers_[1].clip = nullptr;
    layers_[1].weight = 0.0f;
}

void AnimatorComponent::load_state_machine(const std::string& path) {
    state_machine_.load(path);
    use_state_machine_ = true;

    // Set initial clip
    const auto* state = state_machine_.current_state();
    if (state) {
        auto it = clip_map_.find(state->clip);
        if (it != clip_map_.end()) {
            layers_[0].clip = it->second;
            layers_[0].time = 0.0f;
            layers_[0].speed = state->speed;
            layers_[0].weight = 1.0f;
            layers_[0].looping = state->looping;
            layers_[0].context->Invalidate();
        } else {
            FJELL_CORE_WARN("Animator: initial state references unknown clip '{}'", state->clip);
        }
    }

    layers_[1].clip = nullptr;
    layers_[1].weight = 0.0f;
}

void AnimatorComponent::set_clip(const std::string& name, const AnimationClip* clip) {
    clip_map_[name] = clip;
}

void AnimatorComponent::set_param(const std::string& name, float value) {
    state_machine_.set_param(name, value);
}

float AnimatorComponent::get_param(const std::string& name) const {
    return state_machine_.get_param(name);
}

void AnimatorComponent::set_event_track(const std::string& clip_path, AnimationEventTrack track) {
    event_tracks_.insert_or_assign(clip_path, std::move(track));
}

AnimationEventTrack* AnimatorComponent::event_track(const std::string& clip_path) {
    auto it = event_tracks_.find(clip_path);
    return it != event_tracks_.end() ? &it->second : nullptr;
}

// ── Per-frame update ────────────────────────────────────────────────────

void AnimatorComponent::sample_layer(AnimationLayer& layer, float dt) {
    if (!layer.clip) return;

    float prev_time = layer.time;
    float duration = layer.clip->duration();

    // Advance time
    layer.time += dt * layer.speed;

    if (layer.looping) {
        if (duration > 0.0f) {
            while (layer.time >= duration) layer.time -= duration;
            while (layer.time < 0.0f) layer.time += duration;
        }
    } else {
        if (layer.time >= duration) layer.time = duration;
        if (layer.time < 0.0f) layer.time = 0.0f;
    }

    // Sample
    float ratio = (duration > 0.0f) ? layer.time / duration : 0.0f;

    ozz::animation::SamplingJob job;
    job.animation = &layer.clip->raw();
    job.context = layer.context.get();
    job.ratio = ratio;
    job.output = ozz::make_span(layer.locals);

    if (!job.Run()) {
        FJELL_CORE_WARN("SamplingJob failed");
    }

    // Fire events for this layer
    // Find the clip path to look up the event track
    // (we check all registered tracks since we don't store the path on the layer)
    for (auto& [path, track] : event_tracks_) {
        auto it = clip_map_.find(path);
        if (it != clip_map_.end() && it->second == layer.clip) {
            track.fire_in_range(prev_time, layer.time, duration);
            break;
        }
    }
}

void AnimatorComponent::update(float dt) {
    FJELL_PROFILE_SCOPE_N("animator_update");
    if (!skeleton_ || !layers_[0].clip) return;

    // Evaluate state machine transitions
    if (use_state_machine_) {
        bool transitioned = state_machine_.update(dt);

        if (transitioned) {
            // A new transition started — set up layer[1] with the target clip
            const auto* next = state_machine_.next_state();
            if (next) {
                auto it = clip_map_.find(next->clip);
                if (it != clip_map_.end()) {
                    layers_[1].clip = it->second;
                    layers_[1].time = 0.0f;
                    layers_[1].speed = next->speed;
                    layers_[1].weight = 0.0f;
                    layers_[1].looping = next->looping;
                    layers_[1].context->Invalidate();
                } else {
                    FJELL_CORE_WARN("Animator: transition target references unknown clip '{}'", next->clip);
                }
            }
        }

        // Update crossfade weights
        if (state_machine_.is_transitioning()) {
            float alpha = state_machine_.crossfade_alpha();
            layers_[0].weight = 1.0f - alpha;
            layers_[1].weight = alpha;
        } else {
            // Not transitioning — check if we just finished one
            if (layers_[1].clip) {
                // Transition complete: layer[1] becomes layer[0]
                layers_[0].clip = layers_[1].clip;
                layers_[0].time = layers_[1].time;
                layers_[0].speed = layers_[1].speed;
                layers_[0].looping = layers_[1].looping;
                std::swap(layers_[0].context, layers_[1].context);

                layers_[1].clip = nullptr;
                layers_[1].weight = 0.0f;
            }
            layers_[0].weight = 1.0f;

            // Sync current clip with state machine's current state
            const auto* current = state_machine_.current_state();
            if (current) {
                auto it = clip_map_.find(current->clip);
                if (it != clip_map_.end()) {
                    if (it->second != layers_[0].clip) {
                        layers_[0].clip = it->second;
                        layers_[0].time = 0.0f;
                        layers_[0].speed = current->speed;
                        layers_[0].looping = current->looping;
                        layers_[0].context->Invalidate();
                    }
                } else {
                    FJELL_CORE_WARN("Animator: current state references unknown clip '{}'", current->clip);
                }
            }
        }
    }

    // Sample active layers
    sample_layer(layers_[0], dt);
    if (layers_[1].clip) {
        sample_layer(layers_[1], dt);
    }

    // Blend or pass through
    if (layers_[1].clip && layers_[1].weight > 0.0f) {
        // Crossfade blend
        ozz::animation::BlendingJob::Layer blend_layers[2];
        blend_layers[0].transform = ozz::make_span(layers_[0].locals);
        blend_layers[0].weight = layers_[0].weight;
        blend_layers[1].transform = ozz::make_span(layers_[1].locals);
        blend_layers[1].weight = layers_[1].weight;

        ozz::animation::BlendingJob blend_job;
        blend_job.layers = ozz::make_span(blend_layers);
        blend_job.rest_pose = skeleton_->raw().joint_rest_poses();
        blend_job.output = ozz::make_span(blended_locals_);

        if (!blend_job.Run()) {
            FJELL_CORE_WARN("BlendingJob failed");
        }
    } else {
        // Single clip — copy directly
        std::memcpy(blended_locals_.data(), layers_[0].locals.data(),
                     blended_locals_.size() * sizeof(ozz::math::SoaTransform));
    }

    // Local to model space
    ozz::animation::LocalToModelJob l2m_job;
    l2m_job.skeleton = &skeleton_->raw();
    l2m_job.input = ozz::make_span(blended_locals_);
    l2m_job.output = ozz::make_span(model_matrices_);

    if (!l2m_job.Run()) {
        FJELL_CORE_WARN("LocalToModelJob failed");
    }

    // Compute final skin matrices
    compute_skin_matrices();
}

static glm::mat4 ozz_to_glm(const ozz::math::Float4x4& m) {
    // ozz Float4x4 stores 4 columns as SimdFloat4.
    // Use ozz's portable store to extract each column.
    glm::mat4 result;
    ozz::math::StorePtrU(m.cols[0], &result[0].x);
    ozz::math::StorePtrU(m.cols[1], &result[1].x);
    ozz::math::StorePtrU(m.cols[2], &result[2].x);
    ozz::math::StorePtrU(m.cols[3], &result[3].x);
    return result;
}

void AnimatorComponent::compute_skin_matrices() {
    const auto& inv_bind = skeleton_->inverse_bind_matrices();
    int num_joints = skeleton_->num_joints();

    for (int i = 0; i < num_joints; ++i) {
        skin_matrices_[i] = ozz_to_glm(model_matrices_[i]) * inv_bind[i];
    }
}

} // namespace fjell
