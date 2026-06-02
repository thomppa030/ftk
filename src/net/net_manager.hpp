// net_manager.hpp — Core multiplayer state: connection lifecycle, network
// identity, ownership, and replicated-field sync.
//
// Replication uses per-object shadow copies: each send tick the authority
// memcmp's a node's [[fj::replicated]] fields and transform against the last
// sent bytes and ships only what changed (delta compression).
#pragma once

#include "net/transport.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class Scene;
class ScriptManager;
class ResourceManager;

// Network identity, distinct from a scene node id. Assigned by the server and
// replicated so every peer agrees which object is which.
using NetId = uint32_t;
constexpr NetId NET_ID_NONE = 0;

enum class NetRole : uint8_t {
    none,    // not networked (single-player / edit mode)
    server,  // authoritative host
    client,  // connected to a host
};

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
                           ResourceManager& resources);  // drain + apply remote state
    void collect_and_send(Scene& scene, ScriptManager& scripts);    // serialize + send dirty state

    // ── Spawn / despawn (server-authoritative) ─────────────────────────
    // Server instantiates the preset locally, assigns a NetId, sets ownership,
    // and replicates the spawn to all clients. Returns the new NetId (0 on
    // failure or when called off the server).
    NetId spawn(Scene& scene, ResourceManager& resources,
                const std::string& preset_path, const glm::vec3& position,
                PeerId owner);
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

    // ── Network identity (NetId ↔ scene node id) ───────────────────────
    [[nodiscard]] NetId assign_net_id();  // server only
    void register_net_object(NetId net_id, uint32_t node_id);
    void unregister_net_object(NetId net_id);
    [[nodiscard]] uint32_t node_id_for(NetId net_id) const;
    [[nodiscard]] NetId    net_id_for(uint32_t node_id) const;

    // ── Ownership (who may write a given object) ───────────────────────
    void set_owner(NetId net_id, PeerId peer);
    [[nodiscard]] PeerId owner_of(NetId net_id) const;
    [[nodiscard]] bool   is_local_authority(NetId net_id) const;

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
    };

    // Last-sent bytes of an object's replicated fields, in field order. Used to
    // detect which fields changed since the previous send (delta compression).
    struct ShadowState {
        std::vector<uint8_t> data;
    };

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
                            std::span<const uint8_t> data);
    void apply_spawn(Scene& scene, ResourceManager& resources,
                     std::span<const uint8_t> data);
    void apply_despawn(Scene& scene, std::span<const uint8_t> data);
    void apply_rpc(Scene& scene, ScriptManager& scripts, std::span<const uint8_t> data);

    std::unique_ptr<Transport> transport_;
    NetRole role_{NetRole::none};
    PeerId  local_peer_{SERVER_PEER};
    uint32_t server_tick_{0};

    bool  scene_registered_{false};  // one-time bootstrap guard
    NetId next_net_id_{1};  // NET_ID_NONE (0) reserved
    std::unordered_map<NetId, uint32_t> net_to_node_;
    std::unordered_map<uint32_t, NetId> node_to_net_;
    std::unordered_map<NetId, PeerId>   ownership_;
    std::unordered_map<NetId, ShadowState> shadow_;

    // Receive path stores the scene/scripts for the transport callback, which
    // can't carry them through ENet. Set at the top of receive_and_apply.
    Scene*           rx_scene_{nullptr};
    ScriptManager*   rx_scripts_{nullptr};
    ResourceManager* rx_resources_{nullptr};
};

} // namespace fjell
