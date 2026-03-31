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
    layers_[0].clip_path.clear();
    layers_[0].time = 0.0f;
    layers_[0].speed = speed;
    layers_[0].weight = 1.0f;
    layers_[0].looping = looping;
    layers_[0].context->Invalidate();

    layers_[1].clip = nullptr;
    layers_[1].clip_path.clear();
    layers_[1].weight = 0.0f;
}

void AnimatorComponent::play(const std::string& clip_path, const AnimationClip* clip,
                             bool looping, float speed) {
    play(clip, looping, speed);
    layers_[0].clip_path = clip_path;
    ensure_event_track(clip_path, clip);
}

void AnimatorComponent::load_state_machine(const std::string& path) {
    state_machine_.load(path);
    enable_state_machine();
}

void AnimatorComponent::enable_state_machine() {
    use_state_machine_ = true;

    // Set initial clip (or mark as blend space state)
    const auto* state = state_machine_.current_state();
    if (state) {
        if (state->uses_blend_space()) {
            // Blend space states don't set layers_[0].clip — evaluate_blend_space handles it
            layers_[0].clip = nullptr;
            layers_[0].clip_path.clear();
            layers_[0].speed = state->speed;
            layers_[0].weight = 1.0f;
            layers_[0].looping = state->looping;
        } else {
            auto it = clip_map_.find(state->clip);
            if (it != clip_map_.end()) {
                layers_[0].clip = it->second;
                layers_[0].clip_path = state->clip;
                layers_[0].time = 0.0f;
                layers_[0].speed = state->speed;
                layers_[0].weight = 1.0f;
                layers_[0].looping = state->looping;
                layers_[0].context->Invalidate();
            } else {
                FJELL_CORE_WARN("Animator: initial state references unknown clip '{}'", state->clip);
            }
        }
    }

    layers_[1].clip = nullptr;
    layers_[1].clip_path.clear();
    layers_[1].weight = 0.0f;
}

void AnimatorComponent::set_clip(const std::string& name, const AnimationClip* clip) {
    clip_map_[name] = clip;
    ensure_event_track(name, clip);
}

void AnimatorComponent::load_blend_space_1d(const std::string& path, const BlendSpace1D& bs) {
    blend_spaces_1d_[path] = bs;
}

void AnimatorComponent::load_blend_space_2d(const std::string& path, const BlendSpace2D& bs) {
    blend_spaces_2d_[path] = bs;
}

void AnimatorComponent::resolve_blend_spaces(const std::string& base_dir) {
    for (const auto& state : state_machine_.states()) {
        if (!state.uses_blend_space()) continue;

        const auto& bs_path = state.blend_space;
        std::string resolved_bs = base_dir.empty() ? bs_path : base_dir + "/" + bs_path;

        if (bs_path.ends_with(".fjblend2D")) {
            if (blend_spaces_2d_.contains(bs_path)) continue;
            try {
                auto bs = BlendSpace2D::load(resolved_bs);
                for (const auto& pt : bs.points()) {
                    if (pt.clip.empty() || clip_map_.contains(pt.clip)) continue;
                    std::string resolved_clip = base_dir.empty() ? pt.clip : base_dir + "/" + pt.clip;
                    try {
                        auto clip = std::make_shared<AnimationClip>(AnimationClip::load(resolved_clip));
                        set_clip(pt.clip, clip.get());
                        clips.push_back(std::move(clip));
                        clip_paths.push_back(pt.clip);
                    } catch (const std::exception& e) {
                        FJELL_CORE_WARN("Animator: failed to load blend space clip {}: {}", pt.clip, e.what());
                    }
                }
                load_blend_space_2d(bs_path, std::move(bs));
            } catch (const std::exception& e) {
                FJELL_CORE_WARN("Animator: failed to load blend space 2D {}: {}", bs_path, e.what());
            }
        } else if (bs_path.ends_with(".fjblend1D")) {
            if (blend_spaces_1d_.contains(bs_path)) continue;
            try {
                auto bs = BlendSpace1D::load(resolved_bs);
                for (const auto& pt : bs.points()) {
                    if (pt.clip.empty() || clip_map_.contains(pt.clip)) continue;
                    std::string resolved_clip = base_dir.empty() ? pt.clip : base_dir + "/" + pt.clip;
                    try {
                        auto clip = std::make_shared<AnimationClip>(AnimationClip::load(resolved_clip));
                        set_clip(pt.clip, clip.get());
                        clips.push_back(std::move(clip));
                        clip_paths.push_back(pt.clip);
                    } catch (const std::exception& e) {
                        FJELL_CORE_WARN("Animator: failed to load blend space clip {}: {}", pt.clip, e.what());
                    }
                }
                load_blend_space_1d(bs_path, std::move(bs));
            } catch (const std::exception& e) {
                FJELL_CORE_WARN("Animator: failed to load blend space 1D {}: {}", bs_path, e.what());
            }
        }
    }
}

void AnimatorComponent::set_param(const std::string& name, float value) {
    state_machine_.set_param(name, value);
}

float AnimatorComponent::get_param(const std::string& name) const {
    return state_machine_.get_param(name);
}

bool AnimatorComponent::has_active_blend_space() const {
    if (!use_state_machine_) return false;
    const auto* cur = state_machine_.current_state();
    return cur && cur->uses_blend_space();
}

void AnimatorComponent::set_event_track(const std::string& clip_path, AnimationEventTrack track) {
    event_tracks_.insert_or_assign(clip_path, std::move(track));
}

AnimationEventTrack* AnimatorComponent::event_track(const std::string& clip_path) {
    auto it = event_tracks_.find(clip_path);
    return it != event_tracks_.end() ? &it->second : nullptr;
}

void AnimatorComponent::ensure_event_track(const std::string& path, const AnimationClip* clip) {
    if (path.empty() || !clip || clip->events().empty()) return;
    if (event_tracks_.contains(path)) return;

    AnimationEventTrack track;
    for (const auto& e : clip->events()) {
        track.add(e);
    }
    set_event_track(path, std::move(track));
}

// ── Blend space evaluation ──────────────────────────────────────────────

void AnimatorComponent::evaluate_blend_space(const AnimationState& state, float dt) {
    int num_soa = skeleton_->num_soa_joints();
    int num_joints = skeleton_->num_joints();

    // Try 1D first, then 2D
    auto it1d = blend_spaces_1d_.find(state.blend_space);
    if (it1d != blend_spaces_1d_.end()) {
        const auto& bs = it1d->second;
        float param = state_machine_.get_param(bs.parameter());
        auto result = bs.evaluate(param);

        // Ensure we have enough blend layers
        while (blend_layers_.size() < 2) {
            AnimationLayer bl;
            bl.context = std::make_unique<ozz::animation::SamplingJob::Context>(num_joints);
            bl.locals.resize(num_soa);
            blend_layers_.push_back(std::move(bl));
        }

        // Resolve clips
        const AnimationClip* clip_a = nullptr;
        const AnimationClip* clip_b = nullptr;
        if (result.index_a < bs.size()) {
            auto ca = clip_map_.find(bs.points()[result.index_a].clip);
            if (ca != clip_map_.end()) clip_a = ca->second;
        }
        if (result.index_b < bs.size()) {
            auto cb = clip_map_.find(bs.points()[result.index_b].clip);
            if (cb != clip_map_.end()) clip_b = cb->second;
        }

        // Sample and blend
        int count = 0;
        if (clip_a) {
            if (blend_layers_[0].clip != clip_a) {
                blend_layers_[0].clip = clip_a;
                blend_layers_[0].clip_path = bs.points()[result.index_a].clip;
                blend_layers_[0].context->Invalidate();
            }
            blend_layers_[0].speed = state.speed;
            blend_layers_[0].looping = state.looping;
            blend_layers_[0].weight = result.weight_a;
            sample_layer(blend_layers_[0], dt);
            count = 1;
        }
        if (clip_b && clip_b != clip_a && result.weight_b > 0.0f) {
            if (blend_layers_[1].clip != clip_b) {
                blend_layers_[1].clip = clip_b;
                blend_layers_[1].clip_path = bs.points()[result.index_b].clip;
                blend_layers_[1].context->Invalidate();
            }
            blend_layers_[1].speed = state.speed;
            blend_layers_[1].looping = state.looping;
            blend_layers_[1].weight = result.weight_b;
            sample_layer(blend_layers_[1], dt);
            count = 2;
        }

        if (count == 2) {
            ozz::animation::BlendingJob::Layer ozz_layers[2];
            ozz_layers[0].transform = ozz::make_span(blend_layers_[0].locals);
            ozz_layers[0].weight = blend_layers_[0].weight;
            ozz_layers[1].transform = ozz::make_span(blend_layers_[1].locals);
            ozz_layers[1].weight = blend_layers_[1].weight;

            ozz::animation::BlendingJob job;
            job.layers = ozz::make_span(ozz_layers);
            job.rest_pose = skeleton_->raw().joint_rest_poses();
            job.output = ozz::make_span(layers_[0].locals);
            job.Run();
        } else if (count == 1) {
            std::memcpy(layers_[0].locals.data(), blend_layers_[0].locals.data(),
                        layers_[0].locals.size() * sizeof(ozz::math::SoaTransform));
        }
        return;
    }

    auto it2d = blend_spaces_2d_.find(state.blend_space);
    if (it2d != blend_spaces_2d_.end()) {
        const auto& bs = it2d->second;
        float px = state_machine_.get_param(bs.parameter_x());
        float py = state_machine_.get_param(bs.parameter_y());
        auto result = bs.evaluate({px, py});

        // Ensure we have enough blend layers
        while (blend_layers_.size() < 3) {
            AnimationLayer bl;
            bl.context = std::make_unique<ozz::animation::SamplingJob::Context>(num_joints);
            bl.locals.resize(num_soa);
            blend_layers_.push_back(std::move(bl));
        }

        // Resolve clips, sample, collect valid layers
        int count = 0;
        for (uint32_t i = 0; i < result.count; ++i) {
            uint32_t idx = result.indices[i];
            float w = result.weights[i];
            if (w <= 0.0f || idx >= bs.size()) continue;

            auto it = clip_map_.find(bs.points()[idx].clip);
            if (it == clip_map_.end()) continue;

            if (blend_layers_[count].clip != it->second) {
                blend_layers_[count].clip = it->second;
                blend_layers_[count].clip_path = bs.points()[idx].clip;
                blend_layers_[count].context->Invalidate();
            }
            blend_layers_[count].speed = state.speed;
            blend_layers_[count].looping = state.looping;
            blend_layers_[count].weight = w;
            sample_layer(blend_layers_[count], dt);
            count++;
        }

        if (count >= 2) {
            std::vector<ozz::animation::BlendingJob::Layer> ozz_layers(count);
            for (int i = 0; i < count; ++i) {
                ozz_layers[i].transform = ozz::make_span(blend_layers_[i].locals);
                ozz_layers[i].weight = blend_layers_[i].weight;
            }

            ozz::animation::BlendingJob job;
            job.layers = ozz::make_span(ozz_layers);
            job.rest_pose = skeleton_->raw().joint_rest_poses();
            job.output = ozz::make_span(layers_[0].locals);
            job.Run();
        } else if (count == 1) {
            std::memcpy(layers_[0].locals.data(), blend_layers_[0].locals.data(),
                        layers_[0].locals.size() * sizeof(ozz::math::SoaTransform));
        }
    }
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
    if (!layer.clip_path.empty()) {
        auto it = event_tracks_.find(layer.clip_path);
        if (it != event_tracks_.end()) {
            it->second.fire_in_range(prev_time, layer.time, duration,
                [this](const AnimationEvent& e) { on_event.broadcast(e); });
        }
    }
}

void AnimatorComponent::update(float dt) {
    FJELL_PROFILE_SCOPE_N("animator_update");
    if (!skeleton_) return;

    // Need either a clip or an active blend space state
    if (!layers_[0].clip && !use_state_machine_) return;
    if (!layers_[0].clip && use_state_machine_) {
        const auto* cur = state_machine_.current_state();
        if (!cur || !cur->uses_blend_space()) return;
    }

    // Evaluate state machine transitions
    if (use_state_machine_) {
        bool transitioned = state_machine_.update(dt);

        if (transitioned) {
            // A new transition started — set up layer[1]
            const auto* next = state_machine_.next_state();
            if (next) {
                if (next->uses_blend_space()) {
                    // Blend space target: no single clip, evaluate_blend_space handles it
                    layers_[1].clip = nullptr;
                    layers_[1].clip_path.clear();
                    layers_[1].time = 0.0f;
                    layers_[1].speed = next->speed;
                    layers_[1].weight = 0.0f;
                    layers_[1].looping = next->looping;
                } else {
                    auto it = clip_map_.find(next->clip);
                    if (it != clip_map_.end()) {
                        layers_[1].clip = it->second;
                        layers_[1].clip_path = next->clip;
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
        }

        // Update crossfade weights
        if (state_machine_.is_transitioning()) {
            float alpha = state_machine_.crossfade_alpha();
            layers_[0].weight = 1.0f - alpha;
            layers_[1].weight = alpha;
        } else {
            // Not transitioning — check if we just finished one
            if (layers_[1].clip || (layers_[1].weight > 0.0f)) {
                // Transition complete: layer[1] becomes layer[0]
                if (layers_[1].clip) {
                    layers_[0].clip = layers_[1].clip;
                    layers_[0].clip_path = layers_[1].clip_path;
                }
                layers_[0].time = layers_[1].time;
                layers_[0].speed = layers_[1].speed;
                layers_[0].looping = layers_[1].looping;
                std::swap(layers_[0].context, layers_[1].context);

                layers_[1].clip = nullptr;
                layers_[1].clip_path.clear();
                layers_[1].weight = 0.0f;
            }
            layers_[0].weight = 1.0f;

            // Sync current state
            const auto* current = state_machine_.current_state();
            if (current) {
                if (current->uses_blend_space()) {
                    // Blend space state: clear layer clip, evaluate handles it
                    layers_[0].clip = nullptr;
                    layers_[0].clip_path.clear();
                    layers_[0].speed = current->speed;
                    layers_[0].looping = current->looping;
                } else {
                    auto it = clip_map_.find(current->clip);
                    if (it != clip_map_.end()) {
                        if (it->second != layers_[0].clip) {
                            layers_[0].clip = it->second;
                            layers_[0].clip_path = current->clip;
                            layers_[0].time = 0.0f;
                            layers_[0].speed = current->speed;
                            layers_[0].looping = current->looping;
                            layers_[0].context->Invalidate();
                        }
                    }
                }
            }
        }
    }

    // Sample active layers — blend space states evaluate N clips internally
    if (use_state_machine_) {
        const auto* cur = state_machine_.current_state();
        if (cur && cur->uses_blend_space()) {
            evaluate_blend_space(*cur, dt);
        } else {
            sample_layer(layers_[0], dt);
        }

        if (state_machine_.is_transitioning()) {
            const auto* nxt = state_machine_.next_state();
            if (nxt && nxt->uses_blend_space()) {
                // Evaluate blend space into layers_[1].locals
                // Temporarily swap layers_[0] and layers_[1] so evaluate writes into [1]
                std::swap(layers_[0], layers_[1]);
                evaluate_blend_space(*nxt, dt);
                std::swap(layers_[0], layers_[1]);
            } else if (layers_[1].clip) {
                sample_layer(layers_[1], dt);
            }
        }
    } else {
        sample_layer(layers_[0], dt);
        if (layers_[1].clip) {
            sample_layer(layers_[1], dt);
        }
    }

    // Blend or pass through
    bool has_second = layers_[1].clip || (use_state_machine_ && state_machine_.is_transitioning() &&
                                          state_machine_.next_state() && state_machine_.next_state()->uses_blend_space());
    if (has_second && layers_[1].weight > 0.0f) {
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
