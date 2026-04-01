#include "animation/animator_component.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"

#include <ozz/base/maths/simd_math.h>

#include <algorithm>
#include <cmath>
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
}

Result<> AnimatorComponent::load_state_machine(const std::string& path) {
    auto result = state_machine_.load(path);
    if (!result) return result;
    enable_state_machine();
    return {};
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
            auto bs_result = BlendSpace2D::load(resolved_bs);
            if (!bs_result) {
                FJELL_CORE_WARN("Animator: failed to load blend space 2D {}: {}", bs_path, bs_result.error());
                continue;
            }
            for (const auto& pt : bs_result->points()) {
                if (pt.clip.empty() || clip_map_.contains(pt.clip)) continue;
                std::string resolved_clip = base_dir.empty() ? pt.clip : base_dir + "/" + pt.clip;
                auto clip_result = AnimationClip::load(resolved_clip);
                if (clip_result) {
                    auto clip = std::make_shared<AnimationClip>(std::move(*clip_result));
                    set_clip(pt.clip, clip.get());
                    clips.push_back(std::move(clip));
                    clip_paths.push_back(pt.clip);
                } else {
                    FJELL_CORE_WARN("Animator: failed to load blend space clip {}: {}", pt.clip, clip_result.error());
                }
            }
            load_blend_space_2d(bs_path, std::move(*bs_result));
        } else if (bs_path.ends_with(".fjblend1D")) {
            if (blend_spaces_1d_.contains(bs_path)) continue;
            auto bs_result = BlendSpace1D::load(resolved_bs);
            if (!bs_result) {
                FJELL_CORE_WARN("Animator: failed to load blend space 1D {}: {}", bs_path, bs_result.error());
                continue;
            }
            for (const auto& pt : bs_result->points()) {
                if (pt.clip.empty() || clip_map_.contains(pt.clip)) continue;
                std::string resolved_clip = base_dir.empty() ? pt.clip : base_dir + "/" + pt.clip;
                auto clip_result = AnimationClip::load(resolved_clip);
                if (clip_result) {
                    auto clip = std::make_shared<AnimationClip>(std::move(*clip_result));
                    set_clip(pt.clip, clip.get());
                    clips.push_back(std::move(clip));
                    clip_paths.push_back(pt.clip);
                } else {
                    FJELL_CORE_WARN("Animator: failed to load blend space clip {}: {}", pt.clip, clip_result.error());
                }
            }
            load_blend_space_1d(bs_path, std::move(*bs_result));
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

// ── Blend space evaluation ──────────────────────────────────────────────

// Damp a single parameter toward its target. Returns the smoothed value.
static float damp_param(std::unordered_map<std::string, float>& damped,
                        const std::string& name, float target, float speed, float dt) {
    auto [it, inserted] = damped.try_emplace(name, target);
    if (inserted) return target; // first frame: snap to target

    if (speed <= 0.0f) {
        it->second = target; // instant
    } else {
        // Exponential decay: approaches target smoothly, frame-rate independent.
        // factor = 1 - e^(-speed * dt). At speed=10, ~63% of the gap closes per 0.1s.
        float factor = 1.0f - std::exp(-speed * dt);
        it->second += (target - it->second) * factor;
    }
    return it->second;
}

void AnimatorComponent::evaluate_blend_space(const AnimationState& state, float dt) {
    int num_soa = skeleton_->num_soa_joints();
    int num_joints = skeleton_->num_joints();

    // Try 1D first, then 2D
    auto it1d = blend_spaces_1d_.find(state.blend_space);
    if (it1d != blend_spaces_1d_.end()) {
        const auto& bs = it1d->second;
        float raw = state_machine_.get_param(bs.parameter());
        float param = damp_param(damped_params_, bs.parameter(), raw, bs.damp_speed(), dt);
        auto result = bs.evaluate(param);

        // Ensure we have enough blend layers
        while (blend_layers_.size() < 2) {
            AnimationLayer bl;
            bl.context = std::make_unique<ozz::animation::SamplingJob::Context>(num_joints);
            bl.locals.resize(num_soa);
            blend_layers_.push_back(std::move(bl));
        }

        // Resolve clips — match by clip identity to preserve context across point changes
        const AnimationClip* clip_a = nullptr;
        const AnimationClip* clip_b = nullptr;
        std::string path_a, path_b;
        if (result.index_a < bs.size()) {
            auto ca = clip_map_.find(bs.points()[result.index_a].clip);
            if (ca != clip_map_.end()) { clip_a = ca->second; path_a = bs.points()[result.index_a].clip; }
        }
        if (result.index_b < bs.size() && result.weight_b > 0.0f) {
            auto cb = clip_map_.find(bs.points()[result.index_b].clip);
            if (cb != clip_map_.end() && cb->second != clip_a) { clip_b = cb->second; path_b = bs.points()[result.index_b].clip; }
        }

        // Phase sync: advance shared phase by weighted-average rate
        if (bs.phase_sync() && (clip_a || clip_b)) {
            float weighted_rate = 0.0f;
            float total_weight = 0.0f;
            if (clip_a) {
                float dur = clip_a->duration();
                if (dur > 0.0f) { weighted_rate += result.weight_a * (state.speed / dur); total_weight += result.weight_a; }
            }
            if (clip_b) {
                float dur = clip_b->duration();
                if (dur > 0.0f) { weighted_rate += result.weight_b * (state.speed / dur); total_weight += result.weight_b; }
            }
            if (total_weight > 0.0f) {
                blend_phase_ += dt * (weighted_rate / total_weight);
            }
            if (state.looping) {
                blend_phase_ = std::fmod(blend_phase_, 1.0f);
                if (blend_phase_ < 0.0f) blend_phase_ += 1.0f;
            } else {
                blend_phase_ = std::clamp(blend_phase_, 0.0f, 1.0f);
            }
        }

        // Find layers by clip identity (not position) to avoid context invalidation
        int layer_a = -1, layer_b = -1;
        if (clip_a) {
            for (int l = 0; l < static_cast<int>(blend_layers_.size()); ++l) {
                if (blend_layers_[l].clip == clip_a) { layer_a = l; break; }
            }
            if (layer_a < 0) layer_a = 0; // assign first available
        }
        if (clip_b) {
            for (int l = 0; l < static_cast<int>(blend_layers_.size()); ++l) {
                if (blend_layers_[l].clip == clip_b) { layer_b = l; break; }
            }
            if (layer_b < 0) layer_b = (layer_a == 0) ? 1 : 0;
        }

        int count = 0;
        if (clip_a && layer_a >= 0) {
            auto& bl = blend_layers_[layer_a];
            if (bl.clip != clip_a) {
                bl.clip = clip_a;
                bl.clip_path = path_a;
                bl.context->Invalidate();
            }
            bl.speed = state.speed;
            bl.looping = state.looping;
            bl.weight = result.weight_a;
            if (bs.phase_sync()) sample_layer_at_ratio(bl, blend_phase_);
            else sample_layer(bl, dt);
            count = 1;
        }
        if (clip_b && layer_b >= 0) {
            auto& bl = blend_layers_[layer_b];
            if (bl.clip != clip_b) {
                bl.clip = clip_b;
                bl.clip_path = path_b;
                bl.context->Invalidate();
            }
            bl.speed = state.speed;
            bl.looping = state.looping;
            bl.weight = result.weight_b;
            if (bs.phase_sync()) sample_layer_at_ratio(bl, blend_phase_);
            else sample_layer(bl, dt);
            count = 2;
        }

        if (count == 2) {
            ozz::animation::BlendingJob::Layer ozz_layers[2];
            ozz_layers[0].transform = ozz::make_span(blend_layers_[layer_a].locals);
            ozz_layers[0].weight = result.weight_a;
            ozz_layers[1].transform = ozz::make_span(blend_layers_[layer_b].locals);
            ozz_layers[1].weight = result.weight_b;

            ozz::animation::BlendingJob job;
            job.layers = ozz::make_span(ozz_layers);
            job.rest_pose = skeleton_->raw().joint_rest_poses();
            job.output = ozz::make_span(layers_[0].locals);
            job.Run();
        } else if (count == 1 && layer_a >= 0) {
            std::memcpy(layers_[0].locals.data(), blend_layers_[layer_a].locals.data(),
                        layers_[0].locals.size() * sizeof(ozz::math::SoaTransform));
        }
        return;
    }

    auto it2d = blend_spaces_2d_.find(state.blend_space);
    if (it2d != blend_spaces_2d_.end()) {
        const auto& bs = it2d->second;
        float raw_x = state_machine_.get_param(bs.parameter_x());
        float raw_y = state_machine_.get_param(bs.parameter_y());
        float px = damp_param(damped_params_, bs.parameter_x(), raw_x, bs.damp_speed(), dt);
        float py = damp_param(damped_params_, bs.parameter_y(), raw_y, bs.damp_speed(), dt);
        auto result = bs.evaluate({px, py});

        // Ensure we have enough blend layers
        while (blend_layers_.size() < 3) {
            AnimationLayer bl;
            bl.context = std::make_unique<ozz::animation::SamplingJob::Context>(num_joints);
            bl.locals.resize(num_soa);
            blend_layers_.push_back(std::move(bl));
        }

        // Resolve clips — match by clip identity so layers keep their context
        // across triangle transitions (avoids sampling jumps).
        struct ActiveClip { const AnimationClip* clip; std::string path; float weight; };
        ActiveClip active[3]{};
        int active_count = 0;

        for (uint32_t i = 0; i < result.count; ++i) {
            uint32_t idx = result.indices[i];
            float w = result.weights[i];
            if (w <= 0.0f || idx >= bs.size()) continue;

            auto it = clip_map_.find(bs.points()[idx].clip);
            if (it == clip_map_.end()) continue;

            active[active_count++] = {it->second, bs.points()[idx].clip, w};
        }

        // Phase sync: advance a shared normalized phase by weighted-average playback rate,
        // then sample all clips at the same phase. This keeps foot contacts aligned
        // across clips so blending between e.g. strafe-left and strafe-right is smooth.
        if (bs.phase_sync() && active_count > 0) {
            // Compute weight-averaged phase advance rate
            float weighted_rate = 0.0f;
            float total_weight = 0.0f;
            for (int a = 0; a < active_count; ++a) {
                float dur = active[a].clip->duration();
                if (dur > 0.0f) {
                    weighted_rate += active[a].weight * (state.speed / dur);
                    total_weight += active[a].weight;
                }
            }
            if (total_weight > 0.0f) {
                blend_phase_ += dt * (weighted_rate / total_weight);
            }
            // Wrap phase for looping
            if (state.looping) {
                blend_phase_ = std::fmod(blend_phase_, 1.0f);
                if (blend_phase_ < 0.0f) blend_phase_ += 1.0f;
            } else {
                blend_phase_ = std::clamp(blend_phase_, 0.0f, 1.0f);
            }
        }

        // Find or assign a blend layer for each active clip, preserving context
        int sampled = 0;
        int used_layers[3]{-1, -1, -1};

        for (int a = 0; a < active_count; ++a) {
            // First: find a layer already running this clip
            int found = -1;
            for (int l = 0; l < static_cast<int>(blend_layers_.size()); ++l) {
                if (blend_layers_[l].clip == active[a].clip) {
                    found = l;
                    break;
                }
            }
            // Second: assign an unused layer
            if (found < 0) {
                for (int l = 0; l < static_cast<int>(blend_layers_.size()); ++l) {
                    bool taken = false;
                    for (int u = 0; u < a; ++u) {
                        if (used_layers[u] == l) { taken = true; break; }
                    }
                    if (!taken) {
                        found = l;
                        break;
                    }
                }
            }
            if (found < 0) continue; // shouldn't happen with 3 layers

            used_layers[a] = found;
            auto& bl = blend_layers_[found];
            if (bl.clip != active[a].clip) {
                bl.clip = active[a].clip;
                bl.clip_path = active[a].path;
                bl.context->Invalidate();
            }
            bl.speed = state.speed;
            bl.looping = state.looping;
            bl.weight = active[a].weight;

            if (bs.phase_sync()) {
                sample_layer_at_ratio(bl, blend_phase_);
            } else {
                sample_layer(bl, dt);
            }
            sampled++;
        }

        // Clear layers no longer active so they don't hold stale clips
        for (int l = 0; l < static_cast<int>(blend_layers_.size()); ++l) {
            bool in_use = false;
            for (int a = 0; a < active_count; ++a) {
                if (used_layers[a] == l) { in_use = true; break; }
            }
            if (!in_use) {
                blend_layers_[l].clip = nullptr;
                blend_layers_[l].clip_path.clear();
                blend_layers_[l].weight = 0.0f;
            }
        }

        if (sampled >= 2) {
            std::vector<ozz::animation::BlendingJob::Layer> ozz_layers(sampled);
            int j = 0;
            for (int a = 0; a < active_count; ++a) {
                if (used_layers[a] < 0) continue;
                ozz_layers[j].transform = ozz::make_span(blend_layers_[used_layers[a]].locals);
                ozz_layers[j].weight = active[a].weight;
                j++;
            }

            ozz::animation::BlendingJob job;
            job.layers = ozz::make_span(ozz_layers);
            job.rest_pose = skeleton_->raw().joint_rest_poses();
            job.output = ozz::make_span(layers_[0].locals);
            job.Run();
        } else if (sampled == 1) {
            int l = used_layers[0];
            std::memcpy(layers_[0].locals.data(), blend_layers_[l].locals.data(),
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

    // Fire notifies in the time range
    fire_notifies(layer.clip, prev_time, layer.time, duration);
}

void AnimatorComponent::sample_layer_at_ratio(AnimationLayer& layer, float ratio) {
    if (!layer.clip) return;

    float prev_time = layer.time;
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    float duration = layer.clip->duration();
    layer.time = ratio * duration;

    ozz::animation::SamplingJob job;
    job.animation = &layer.clip->raw();
    job.context = layer.context.get();
    job.ratio = ratio;
    job.output = ozz::make_span(layer.locals);

    if (!job.Run()) {
        FJELL_CORE_WARN("SamplingJob failed");
    }

    // Fire notifies in the time range (fixes phase-sync gap)
    fire_notifies(layer.clip, prev_time, layer.time, duration);
}

void AnimatorComponent::fire_notifies(const AnimationClip* clip, float prev_time,
                                       float current_time, float duration) {
    if (!clip || !owner_ || duration <= 0.0f) return;

    for (const auto& ni : clip->notifies()) {
        if (!ni.instance) continue;

        if (ni.instance->is_state()) {
            // State notify: begin/tick/end based on time range
            auto* state_notify = static_cast<AnimNotifyState*>(ni.instance.get());
            float end = ni.end_time > 0.0f ? ni.end_time : ni.time;
            bool was_in = (prev_time >= ni.time && prev_time < end);
            bool now_in = (current_time >= ni.time && current_time < end);

            if (!was_in && now_in) {
                state_notify->notify_begin(*owner_);
            } else if (was_in && now_in) {
                float dt = current_time - prev_time;
                if (dt < 0.0f) dt += duration; // loop wraparound
                state_notify->notify_tick(*owner_, dt);
            } else if (was_in && !now_in) {
                state_notify->notify_end(*owner_);
            }
        } else {
            // Instant notify: fire when time crosses the notify point
            if (current_time > prev_time) {
                // Normal forward
                if (ni.time > prev_time && ni.time <= current_time) {
                    ni.instance->notify(*owner_);
                }
            } else if (current_time < prev_time) {
                // Loop wraparound
                if (ni.time > prev_time && ni.time <= duration) {
                    ni.instance->notify(*owner_);
                }
                if (ni.time >= 0.0f && ni.time <= current_time) {
                    ni.instance->notify(*owner_);
                }
            }
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
