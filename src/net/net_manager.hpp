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
    void set_tick_rate(float hz) {
        if (hz <= 0.0f) return;
        tick_interval_ = 1.0f / hz;
        interp_delay_  = 2.0f / hz;   // render ~2 ticks behind newest
    }

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
    // Packet types (first byte of every payload).
    enum class PacketType : uint8_t {
        state_update = 1,  // delta of replicated fields + transform (unreliable)
        spawn        = 2,  // instantiate a preset on the client (reliable)
        despawn      = 3,  // remove a networked object on the client (reliable)
        rpc_call     = 4,  // invoke a [[fj::rpc]] handler on the target (reliable)
        welcome      = 5,  // server → new client: "your assigned peer id" (reliable)
    };

    // Last-sent bytes of an object's replicated fields, in field order. Used to
    // detect which fields changed since the previous send (delta compression).
    struct ShadowState {
        std::vector<uint8_t> data;
    };

    // A short history of received transform samples for a remote object, each
    // stamped on a synthetic timeline (one tick_interval_ apart, jitter-free).
    // Each frame the render-time transform is interpolated between the two
    // samples that bracket `net_time_`, which trails interp_delay_ behind the
    // newest. A multi-sample ring (not just the last two) gives render_time room
    // to sit several ticks back without clamping, so jitter is absorbed instead
    // of producing freezes/snaps.
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

    // Push a freshly-received transform sample for a remote object into its ring.
    void push_interp_sample(NetId net_id, const glm::vec3& pos, const glm::quat& rot);

    // v1 bootstrap: register every scene node that has a replicated component,
    // assigning NetIds by ascending node id so host and client agree without a
    // handshake. Spawn/despawn replication (PR 4) supersedes this for dynamic
    // objects. Runs once per session, when the registry is still empty.
    void ensure_scene_registered(Scene& scene, ScriptManager& scripts);

    void handle_connect(PeerId peer);
    void handle_disconnect(PeerId peer);
    void handle_receive(Scene& scene, ScriptManager& scripts, ResourceManager& resources,
                        PeerId peer, std::span<const uint8_t> data, Channel channel);
    void apply_state_update(Scene& scene, ScriptManager& scripts,
                            PeerId sender, std::span<const uint8_t> data);
    void apply_spawn(Scene& scene, ResourceManager& resources,
                     std::span<const uint8_t> data);  // uses rx_project_ for path resolution
    [[nodiscard]] std::vector<uint8_t> build_spawn_packet(NetId net_id) const;
    void apply_despawn(Scene& scene, std::span<const uint8_t> data);
    void apply_rpc(Scene& scene, ScriptManager& scripts, std::span<const uint8_t> data);
    void apply_welcome(std::span<const uint8_t> data);  // client learns its own peer id

    std::unique_ptr<Transport> transport_;
    NetRole role_{NetRole::none};
    PeerId  local_peer_{SERVER_PEER};
    uint32_t server_tick_{0};

    NetLaunchMode launch_mode_{NetLaunchMode::offline};
    std::string   join_address_{"127.0.0.1"};

    bool  scene_registered_{false};  // one-time bootstrap guard
    NetScene net_scene_;  // NetId<->node, ownership, authority/possession queries
    std::unordered_map<NetId, ShadowState> shadow_;

    // Server: remembers how each networked object was spawned, so a late-joining
    // client can be brought up to date (replay spawns it missed). Spawn position
    // is the initial one; the subsequent state_update stream carries live motion.
    struct SpawnInfo {
        std::string preset_path;
        PeerId      owner{SERVER_PEER};
        glm::vec3   position{0.0f};
    };
    std::unordered_map<NetId, SpawnInfo> spawn_info_;

    // Client-side prediction reconciliation for OWNED objects. The owning client
    // moves its object locally each tick (responsive input); when the server's
    // authoritative state arrives we don't snap — we ease toward it (hiding small
    // divergences) unless the error is large (teleport/respawn), which snaps.
    struct PendingCorrection {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        bool active{false};
    };
    std::unordered_map<NetId, PendingCorrection> corrections_;
    float correction_rate_{10.0f};   // ease ~rate*dt toward server state per frame
    float snap_threshold_{2.0f};     // metres of error above which we hard-snap

    // Client-side render-time interpolation of remote objects.
    //
    // Samples are stamped on a synthetic timeline spaced by the nominal send
    // interval, NOT by arrival time — network jitter (two packets in one frame,
    // or a late one) would otherwise collapse or stretch the gap between samples
    // and make the blend snap or freeze. net_time_ (the render clock) chases that
    // timeline interp_delay_ behind, so there's always a bracketing pair.
    std::unordered_map<NetId, InterpBuffer> interp_;
    float net_time_{0.0f};          // shared render clock (drift-corrected, not raw dt sum)
    float newest_sample_t_{0.0f};   // timeline stamp of the most recent sample, any object
    bool  net_time_primed_{false};  // false until the first sample seeds net_time_
    float tick_interval_{1.0f / 30.0f};  // expected server send period (from config)
    float interp_delay_{2.0f / 30.0f};   // render ~2 ticks behind newest for jitter headroom

    // Receive path stores the scene/scripts for the transport callback, which
    // can't carry them through ENet. Set at the top of receive_and_apply.
    Scene*           rx_scene_{nullptr};
    ScriptManager*   rx_scripts_{nullptr};
    ResourceManager* rx_resources_{nullptr};
    const Project*   rx_project_{nullptr};  // for resolving asset paths in spawned presets
};

} // namespace fjell
