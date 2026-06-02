// net_manager.hpp — Core multiplayer state: connection lifecycle, network
// identity, and ownership. Owned by Engine, shared by the net subsystems.
//
// PR 1 establishes the lifecycle and identity bookkeeping. State replication
// (receive_and_apply / collect_and_send bodies) lands in later PRs; here they
// only pump the transport.
#pragma once

#include "net/transport.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace fjell {

class Scene;

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
    void receive_and_apply(Scene& scene);  // drains transport, applies remote state
    void collect_and_send(Scene& scene);    // serializes + sends dirty state

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
    void handle_connect(PeerId peer);
    void handle_disconnect(PeerId peer);
    void handle_receive(PeerId peer, std::span<const uint8_t> data, Channel channel);

    std::unique_ptr<Transport> transport_;
    NetRole role_{NetRole::none};
    PeerId  local_peer_{SERVER_PEER};

    NetId next_net_id_{1};  // NET_ID_NONE (0) reserved
    std::unordered_map<NetId, uint32_t> net_to_node_;
    std::unordered_map<uint32_t, NetId> node_to_net_;
    std::unordered_map<NetId, PeerId>   ownership_;
};

} // namespace fjell
