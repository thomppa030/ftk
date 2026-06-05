// prediction_state.hpp — Client/peer-side smoothing of networked transforms.
//
// Two jobs, both about how a peer *consumes* the discrete state stream:
//   - Remote (non-owned) objects: render-time interpolation between received
//     snapshots, on a jitter-free synthetic timeline trailing interp_delay_
//     behind the newest sample. Both peers do this for objects they don't own.
//   - Owned objects: smoothed correction — ease the predicted transform toward
//     the authority's, hard-snapping only on large divergence (teleport/respawn).
//
// The timeline anchoring (new joiners / idle-resume re-anchor one interp delay
// ahead of the render clock) is subtle and validated — see
// project_net_component_replication. Behaviour here must not change without a
// deliberate, separately-tested decision.
#pragma once

#include "net/net_types.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <unordered_map>

namespace fjell {

class Scene;
class NetScene;
struct Node3D;

class PredictionState {
public:
    // Match the interpolation timeline + render delay to the server's send rate.
    void set_tick_rate(float hz) {
        if (hz <= 0.0f) return;
        tick_interval_ = 1.0f / hz;
        interp_delay_  = 2.0f / hz;  // render ~2 ticks behind newest
    }

    // Remote object: push a freshly-received transform sample into its ring.
    void push_sample(NetId net_id, const glm::vec3& pos, const glm::quat& rot);

    // Owned object: reconcile a predicted transform toward the authority. Snaps
    // on large error (teleport/respawn), otherwise records a pending correction
    // that apply() eases toward. Writes directly to `spatial` on a snap.
    void reconcile_owned(NetId net_id, Node3D& spatial, const glm::vec3& pos,
                         const glm::quat& rot);

    // Per-frame: advance the render clock, ease owned objects toward their
    // corrections, and write interpolated transforms for remote objects.
    void apply(Scene& scene, const NetScene& net_scene, NetRole role,
               PeerId local_peer, float dt);

    void erase(NetId net_id) {
        interp_.erase(net_id);
        corrections_.erase(net_id);
    }

    void clear() {
        interp_.clear();
        corrections_.clear();
        net_time_ = 0.0f;
        newest_sample_t_ = 0.0f;
        net_time_primed_ = false;
    }

private:
    // A short history of received transform samples for a remote object, each
    // stamped on a synthetic timeline (one tick_interval_ apart, jitter-free).
    // The render-time transform is interpolated between the two samples that
    // bracket net_time_, which trails interp_delay_ behind the newest. A
    // multi-sample ring (not just the last two) gives render_time room to sit
    // several ticks back without clamping, absorbing jitter instead of snapping.
    struct InterpSnapshot {
        float      t{0.0f};
        glm::vec3  position{0.0f};
        glm::quat  rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };
    static constexpr size_t INTERP_HISTORY = 8;
    struct InterpBuffer {
        InterpSnapshot samples[INTERP_HISTORY];  // ring; newest at (head-1)
        size_t head{0};   // next write index
        size_t count{0};  // number of valid samples (<= INTERP_HISTORY)
    };

    // Client-side prediction reconciliation for OWNED objects. The owning client
    // moves its object locally each tick (responsive input); when the server's
    // authoritative state arrives we don't snap — we ease toward it (hiding small
    // divergences) unless the error is large (teleport/respawn), which snaps.
    struct PendingCorrection {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        bool active{false};
    };

    std::unordered_map<NetId, InterpBuffer> interp_;
    std::unordered_map<NetId, PendingCorrection> corrections_;

    float net_time_{0.0f};          // shared render clock (drift-corrected, not raw dt sum)
    float newest_sample_t_{0.0f};   // timeline stamp of the most recent sample, any object
    bool  net_time_primed_{false};  // false until the first sample seeds net_time_
    float tick_interval_{1.0f / 30.0f};  // expected server send period (from config)
    float interp_delay_{2.0f / 30.0f};   // render ~2 ticks behind newest for jitter headroom
    float correction_rate_{10.0f};   // ease ~rate*dt toward server state per frame
    float snap_threshold_{2.0f};     // metres of error above which we hard-snap
};

} // namespace fjell
