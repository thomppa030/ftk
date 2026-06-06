// replication_graph.hpp — Builds and applies the per-object state stream.
//
// On the authority side, collect_and_send walks the networked objects it
// authors, diffs each one's replicated script fields + transform + engine-
// component section against a per-object shadow, and ships only what changed.
// On the receiving side, apply_state_update parses that stream, applies accepted
// objects (fields directly, transforms via PredictionState), and — on the server
// — relays client-authored updates to the other clients.
//
// Owns the shadow copies + the send tick + the one-time scene registration guard.
#pragma once

#include "net/net_types.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace fjell {

class Scene;
class ScriptManager;
class NetScene;
class PredictionState;
class Transport;

class ReplicationGraph {
public:
    // One-time: assign NetIds to every scene object that carries a replicated
    // component, in a stable (sorted) order both peers compute identically.
    void ensure_scene_registered(Scene& scene, ScriptManager& scripts,
                                 NetScene& net_scene);

    // Authority side: serialize + send the dirty state of the objects this peer
    // authors. Flushes the transport.
    void collect_and_send(Scene& scene, ScriptManager& scripts, NetScene& net_scene,
                          Transport* transport, NetRole role, PeerId local_peer);

    // Receive side: apply a state_update packet. `sender` is the peer it came
    // from; `local_peer` is this peer's own id (for the possession check).
    void apply_state_update(Scene& scene, ScriptManager& scripts, NetScene& net_scene,
                            PredictionState& prediction, Transport* transport,
                            NetRole role, PeerId sender, PeerId local_peer,
                            std::span<const uint8_t> data);

    void erase(NetId net_id) { shadow_.erase(net_id); }
    void clear() {
        shadow_.clear();
        server_tick_ = 0;
        scene_registered_ = false;
    }

private:
    // Last-sent bytes of an object's replicated state (fields + transform +
    // component section), used to detect what changed since the previous send.
    struct ShadowState {
        std::vector<uint8_t> data;
    };
    std::unordered_map<NetId, ShadowState> shadow_;
    uint32_t server_tick_{0};
    bool     scene_registered_{false};  // one-time bootstrap guard
};

} // namespace fjell
