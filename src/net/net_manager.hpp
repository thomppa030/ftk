// net_manager.hpp — Core multiplayer state: connection lifecycle, network
// identity, ownership, and replicated-field sync.
//
// Replication uses per-object shadow copies: each send tick the authority
// memcmp's a node's [[fj::replicated]] fields and transform against the last
// sent bytes and ships only what changed (delta compression).
#pragma once

#include "net/transport.hpp"
#include "net/net_types.hpp"
#include "net/net_scene.hpp"
#include "net/prediction_state.hpp"
#include "net/spawn_replicator.hpp"
#include "net/rpc_router.hpp"
#include "net/replication_graph.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class Scene;
class ScriptManager;
class ResourceManager;
class Project;

class NetManager {
public:
    NetManager();
    ~NetManager();
    NetManager(const NetManager&) = delete;
    NetManager& operator=(const NetManager&) = delete;
    NetManager(NetManager&&) = delete;
    NetManager& operator=(NetManager&&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────────
    void host(uint16_t port, uint32_t max_clients);
    void join(const std::string& address, uint16_t port);
    void shutdown();

    // ── Per-frame pumps (called by the net subsystems) ─────────────────
    void receive_and_apply(Scene& scene, ScriptManager& scripts,
                           ResourceManager& resources,
                           const Project* project);  // drain + apply remote state
    void collect_and_send(Scene& scene, ScriptManager& scripts);    // serialize + send dirty state

    // Advance the interpolation clock and write each remote (non-authority)
    // object's transform toward `net_time_ - interp_delay_`, so clients render
    // smooth motion between discrete 30 Hz snapshots instead of snapping. Called
    // by NetReceiveSubsystem right after receive_and_apply, before world matrices.
    void interpolate_remotes(Scene& scene, float dt);

    // Tell the interpolator the server's send rate so the synthetic sample
    // timeline (and the render delay) match the real packet cadence. Wired from
    // project config alongside NetSendSubsystem::set_tick_rate.
    void set_tick_rate(float hz) { prediction_.set_tick_rate(hz); }

    // ── Spawn / despawn (server-authoritative) ─────────────────────────
    // Server instantiates the preset locally, assigns a NetId, sets ownership,
    // and replicates the spawn to all clients. Returns the new NetId (0 on
    // failure or when called off the server).
    NetId spawn(Scene& scene, ResourceManager& resources,
                const std::string& preset_path, const glm::vec3& position,
                PeerId owner, const Project* project);
    // Server destroys a networked object locally and tells all clients.
    void destroy(Scene& scene, NetId net_id);

    // ── RPC ────────────────────────────────────────────────────────────
    // Called by the generated send-wrapper. Routes an RPC_CALL by target:
    // server-target → to the host; clients-target → broadcast to clients;
    // all → run locally + forward. `node_id` is the calling component's node;
    // `rpc_index` indexes its FjellRpcDesc[].
    void send_rpc(Scene& scene, ScriptManager& scripts, uint32_t node_id,
                  uint16_t rpc_index, const uint8_t* payload, uint32_t size);

    // ── Queries ────────────────────────────────────────────────────────
    [[nodiscard]] bool    is_server() const { return role_ == NetRole::server; }
    [[nodiscard]] bool    is_client() const { return role_ == NetRole::client; }
    [[nodiscard]] bool    is_connected() const { return role_ != NetRole::none; }
    [[nodiscard]] NetRole role() const { return role_; }
    [[nodiscard]] PeerId  local_peer() const { return local_peer_; }

    // ── Observability (for the net panel) ───────────────────────────────
    [[nodiscard]] TransportStats stats() const {
        return transport_ ? transport_->stats() : TransportStats{};
    }
    [[nodiscard]] std::vector<PeerInfo> peers() const {
        return transport_ ? transport_->peers() : std::vector<PeerInfo>{};
    }
    // Inject artificial latency/jitter/loss for testing (no-op when offline).
    void set_conditioning(float latency_ms, float jitter_ms, float loss_pct) {
        if (transport_) transport_->set_conditioning(latency_ms, jitter_ms, loss_pct);
    }

    // ── Launch mode (editor → script) ───────────────────────────────────
    // Set by the editor's Net dropdown before Play; read by the game script in
    // begin_play to decide host/join/offline. Defaults track FJELL_NET_MODE so
    // the env-var workflow still works when launched headless. The engine itself
    // never acts on these.
    void set_launch_mode(NetLaunchMode mode) { launch_mode_ = mode; }
    [[nodiscard]] NetLaunchMode launch_mode() const { return launch_mode_; }
    void set_join_address(std::string address) { join_address_ = std::move(address); }
    [[nodiscard]] const std::string& join_address() const { return join_address_; }

    // ── Network identity (NetId ↔ scene node id) ───────────────────────
    [[nodiscard]] NetId assign_net_id();  // server only
    void register_net_object(NetId net_id, uint32_t node_id);
    void unregister_net_object(NetId net_id);
    [[nodiscard]] uint32_t node_id_for(NetId net_id) const;
    [[nodiscard]] NetId    net_id_for(uint32_t node_id) const;

    // ── Ownership (who may write a given object) ───────────────────────
    void set_owner(NetId net_id, PeerId peer);
    [[nodiscard]] PeerId owner_of(NetId net_id) const;

    // "May this peer write the object's authoritative state?" — true on the
    // server for everything (it owns the truth) and on a client for objects it
    // owns. Used to gate replication writes / prediction.
    [[nodiscard]] bool is_local_authority(NetId net_id) const;

    // "Does the local *player* control this object?" — true only when the object's
    // owner equals this peer's id. Distinct from is_local_authority: the server is
    // authoritative over every object, but the host player only *possesses* the
    // ones it owns. Use this for camera/input possession, NOT is_local_authority.
    [[nodiscard]] bool is_owned_by_local(NetId net_id) const;

    // Connection events surfaced to scripts (wired to ScriptComponent in Step 7).
    // PR 1 just logs; the engine sets these to forward into script callbacks later.
    std::function<void(PeerId)> on_peer_connected;
    std::function<void(PeerId)> on_peer_disconnected;

private:
    void handle_connect(PeerId peer);
    void handle_disconnect(PeerId peer);
    void handle_receive(const NetContext& ctx, PeerId peer,
                        std::span<const uint8_t> data, Channel channel);
    void apply_welcome(std::span<const uint8_t> data);  // client learns its own peer id

    std::unique_ptr<Transport> transport_;
    NetRole role_{NetRole::none};
    PeerId  local_peer_{SERVER_PEER};

    NetLaunchMode launch_mode_{NetLaunchMode::offline};
    std::string   join_address_{"127.0.0.1"};

    NetScene net_scene_;  // NetId<->node, ownership, authority/possession queries

    // Per-object state collect/apply + shadow diff + scene registration.
    ReplicationGraph replication_;

    // Server-authoritative spawn/despawn + late-join replay (owns spawn records).
    SpawnReplicator spawn_replicator_;

    // Routes [[fj::rpc]] calls by target (stateless).
    RpcRouter rpc_router_;

    // Render-time interpolation of remote objects + smoothed correction of owned
    // (predicted) objects. Owns the interp buffers, render clock, and tuning.
    PredictionState prediction_;

    // Points at a stack-local NetContext only for the duration of a poll() (set
    // at the top of receive_and_apply, cleared after). ENet's receive callback
    // fires inside poll() and can't carry the context, so it reads it from here.
    const NetContext* rx_ctx_{nullptr};
};

} // namespace fjell
