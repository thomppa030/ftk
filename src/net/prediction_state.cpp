#include "net/prediction_state.hpp"

#include "net/net_scene.hpp"
#include "scene/scene.hpp"
#include "scene/node.hpp"

#include <algorithm>

namespace fjell {

void PredictionState::push_sample(NetId net_id, const glm::vec3& pos,
                                  const glm::quat& rot) {
    InterpBuffer& buf = interp_[net_id];

    // Stamp on a per-object monotonic timeline: normally exactly one nominal tick
    // past this object's previous sample, independent of arrival jitter.
    //
    // But re-anchor ahead of the render clock whenever the object's own newest
    // sample has fallen behind it. That happens (a) for a brand-new object that
    // joins an already-running session, and (b) for an existing object that went
    // idle — no transform change means no samples, so the render clock marches
    // past its stale ring while net_time_primed_ stays true and buf.count stays
    // > 0. In both cases, continuing from "newest + tick" would stamp the new
    // sample in the past, leaving the object frozen on a stale sample until the
    // clock crawled back up to it — the "cranky for a few seconds, then smooth"
    // feel. Anchoring to net_time_ + interp_delay_ puts the fresh sample exactly
    // one interp delay ahead of render_time, the steady-state relationship.
    float t;
    float prev_newest = -1.0f;
    if (buf.count > 0) {
        const size_t newest = (buf.head + INTERP_HISTORY - 1) % INTERP_HISTORY;
        prev_newest = buf.samples[newest].t;
    }

    if (!net_time_primed_) {
        // Very first sample of the session: anchor the shared timeline here.
        t = std::max(newest_sample_t_, tick_interval_);
    } else if (prev_newest >= net_time_) {
        // Continuous stream: keep the monotonic per-object cadence.
        t = prev_newest + tick_interval_;
    } else {
        // New or resuming-after-idle: re-anchor just ahead of the render clock.
        t = net_time_ + interp_delay_;
    }

    buf.samples[buf.head] = {t, pos, rot};
    buf.head = (buf.head + 1) % INTERP_HISTORY;
    if (buf.count < INTERP_HISTORY) ++buf.count;

    newest_sample_t_ = std::max(newest_sample_t_, t);

    // Seed the render clock the first time we ever see a sample: place it
    // interp_delay_ behind, so it starts inside the (about-to-fill) buffer.
    if (!net_time_primed_) {
        net_time_ = t - interp_delay_;
        net_time_primed_ = true;
    }
}

void PredictionState::reconcile_owned(NetId net_id, Node3D& spatial,
                                      const glm::vec3& pos, const glm::quat& rot) {
    // (client only) reconcile our prediction toward the authority.
    if (glm::distance(spatial.transform.position, pos) > snap_threshold_) {
        spatial.transform.position = pos;
        spatial.transform.rotation = rot;
        corrections_[net_id].active = false;
    } else {
        corrections_[net_id] = {pos, rot, true};
    }
}

void PredictionState::apply(Scene& scene, const NetScene& net_scene, NetRole role,
                            PeerId local_peer, float dt) {
    // Both peers interpolate the objects they don't own: the client smooths the
    // host's player, the server smooths each client's player. Offline does nothing.
    if (role == NetRole::none || !net_time_primed_) return;

    // Advance the render clock at a constant nominal rate (one second of timeline
    // per real second). It must NOT chase the newest stamp directly: that target
    // steps up a whole tick on each (jittery) arrival, so a proportional pull
    // springs back and forth → rubberbanding. Instead run free and apply only a
    // gentle correction when buffer depth drifts outside a dead-band around
    // interp_delay_. Inside the band, motion is perfectly linear.
    net_time_ += dt;
    const float depth = newest_sample_t_ - net_time_;  // how far behind newest we render
    const float lo = interp_delay_ - tick_interval_;   // dead-band: ±1 tick of slack
    const float hi = interp_delay_ + tick_interval_;
    if (depth < lo) {
        net_time_ -= (lo - depth) * std::min(1.0f, 2.0f * dt);  // about to starve, ease back
    } else if (depth > hi) {
        net_time_ += (depth - hi) * std::min(1.0f, 2.0f * dt);  // buffer grew, ease forward
    }
    const float render_time = net_time_;

    // Reconcile predicted (owned) objects toward the last authoritative state.
    // Easing the predicted base toward the server hides small divergences; the
    // script's own movement this frame still applies on top, so input stays
    // responsive. Large errors were already hard-snapped in reconcile_owned.
    const float blend = std::min(1.0f, correction_rate_ * dt);
    for (auto& [net_id, corr] : corrections_) {
        if (!corr.active) continue;
        uint32_t node_id = net_scene.node_id_for(net_id);
        Node* node = (node_id != 0) ? scene.find(node_id) : nullptr;
        Node3D* spatial = node ? node->as_spatial() : nullptr;
        if (!spatial) continue;
        spatial->transform.position =
            glm::mix(spatial->transform.position, corr.position, blend);
        spatial->transform.rotation =
            glm::slerp(spatial->transform.rotation, corr.rotation, blend);
    }

    for (auto& [net_id, buf] : interp_) {
        if (net_scene.is_owned_by_local(net_id, role, local_peer)) continue;  // owned: driven by input/prediction
        if (buf.count == 0) continue;

        uint32_t node_id = net_scene.node_id_for(net_id);
        Node* node = (node_id != 0) ? scene.find(node_id) : nullptr;
        Node3D* spatial = node ? node->as_spatial() : nullptr;
        if (!spatial) continue;

        // Find the pair of samples that brackets render_time. Samples are ordered
        // oldest→newest as (head, head+1, ...) modulo the ring.
        const size_t oldest = buf.head + (INTERP_HISTORY - buf.count);
        const InterpSnapshot& first = buf.samples[oldest % INTERP_HISTORY];
        const InterpSnapshot& last  =
            buf.samples[(buf.head + INTERP_HISTORY - 1) % INTERP_HISTORY];

        glm::vec3 pos = last.position;
        glm::quat rot = last.rotation;
        if (render_time <= first.t) {
            pos = first.position;  // before history → hold oldest (buffer filling)
            rot = first.rotation;
        } else if (render_time < last.t) {
            for (size_t k = 1; k < buf.count; ++k) {
                const InterpSnapshot& a = buf.samples[(oldest + k - 1) % INTERP_HISTORY];
                const InterpSnapshot& b = buf.samples[(oldest + k) % INTERP_HISTORY];
                if (render_time <= b.t && b.t > a.t) {
                    const float alpha = std::clamp((render_time - a.t) / (b.t - a.t),
                                                   0.0f, 1.0f);
                    pos = glm::mix(a.position, b.position, alpha);
                    rot = glm::slerp(a.rotation, b.rotation, alpha);
                    break;
                }
            }
        }
        // else render_time >= last.t → hold newest (starved; no extrapolation).

        spatial->transform.position = pos;
        spatial->transform.rotation = rot;
    }
}

} // namespace fjell
