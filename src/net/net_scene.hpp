// net_scene.hpp — Network identity registry: NetId <-> scene node mapping,
// ownership, and the authority/possession queries.
//
// This is the single source of truth for "which networked object is which" and
// "who controls it". Authority and possession are DIFFERENT and both live here,
// documented together, so they can't drift apart again (they were conflated in
// two separate bugs before this split — see feedback_authority_vs_possession):
//
//   is_local_authority — "may I write this object's authoritative state?"
//       TRUE on the server for EVERY object; on a client only for objects it owns.
//   is_owned_by_local  — "does the local PLAYER control this object?" (camera/
//       input possession, and "driven locally, skip remote interpolation").
//       TRUE only when the object's owner == this peer's id.
//
// The queries take role + local_peer as arguments rather than storing them: that
// state belongs to the session, and passing it keeps NetScene free of a
// dependency on the session/transport layer.
#pragma once

#include "net/net_types.hpp"

#include <cstdint>
#include <unordered_map>

namespace fjell {

class NetScene {
public:
    // Reset to the just-constructed state (called on shutdown).
    void clear() {
        next_net_id_ = 1;
        net_to_node_.clear();
        node_to_net_.clear();
        ownership_.clear();
    }

    // ── NetId assignment (server only) ──────────────────────────────────
    [[nodiscard]] NetId assign_net_id() { return next_net_id_++; }

    // ── Registration ────────────────────────────────────────────────────
    void register_object(NetId net_id, uint32_t node_id) {
        net_to_node_[net_id] = node_id;
        node_to_net_[node_id] = net_id;
    }

    // Drop the identity + ownership entry for an object. Other per-object state
    // (shadow, interp, spawn info) lives in other components and is cleared by
    // the facade alongside this call.
    void unregister_object(NetId net_id) {
        auto it = net_to_node_.find(net_id);
        if (it != net_to_node_.end()) {
            node_to_net_.erase(it->second);
            net_to_node_.erase(it);
        }
        ownership_.erase(net_id);
    }

    [[nodiscard]] uint32_t node_id_for(NetId net_id) const {
        auto it = net_to_node_.find(net_id);
        return it != net_to_node_.end() ? it->second : 0;
    }

    [[nodiscard]] NetId net_id_for(uint32_t node_id) const {
        auto it = node_to_net_.find(node_id);
        return it != node_to_net_.end() ? it->second : NET_ID_NONE;
    }

    [[nodiscard]] const std::unordered_map<NetId, uint32_t>& objects() const {
        return net_to_node_;
    }

    // ── Ownership ───────────────────────────────────────────────────────
    void set_owner(NetId net_id, PeerId peer) { ownership_[net_id] = peer; }

    [[nodiscard]] PeerId owner_of(NetId net_id) const {
        auto it = ownership_.find(net_id);
        return it != ownership_.end() ? it->second : SERVER_PEER;
    }

    // ── Authority / possession (see file header) ────────────────────────
    [[nodiscard]] bool is_local_authority(NetId net_id, NetRole role,
                                          PeerId local_peer) const {
        // The server is authoritative over everything. A client has authority
        // only over objects EXPLICITLY owned by its peer; everything else (the
        // default) belongs to the server, so the client applies the server's
        // state to it. local_peer is unreliable until peer-id assignment, so we
        // key strictly off an explicit ownership entry, not the default.
        if (role == NetRole::server) return true;
        if (role == NetRole::client) {
            auto it = ownership_.find(net_id);
            return it != ownership_.end() && it->second == local_peer &&
                   local_peer != SERVER_PEER;
        }
        return true;  // single-player: always authoritative
    }

    [[nodiscard]] bool is_owned_by_local(NetId net_id, NetRole role,
                                         PeerId local_peer) const {
        // Possession is about the local player, not server authority: the
        // object's owner must equal this peer's id. On the host, local_peer ==
        // SERVER_PEER, so the host possesses objects it spawned for itself
        // (owner 0) but NOT a client's player (owner 1). Offline everything is
        // "owned" by the player.
        if (role == NetRole::none) return true;
        return owner_of(net_id) == local_peer;
    }

private:
    NetId next_net_id_{1};  // NET_ID_NONE (0) reserved
    std::unordered_map<NetId, uint32_t> net_to_node_;
    std::unordered_map<uint32_t, NetId> node_to_net_;
    std::unordered_map<NetId, PeerId>   ownership_;
};

} // namespace fjell
