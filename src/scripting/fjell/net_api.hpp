// net_api.hpp — Static networking API for game scripts.
//
// Mirrors scene / input: the engine exports fjell_net_* symbols
// (resolved at dlopen via -rdynamic), and this header wraps them in a
// convenient static struct. Covers connection lifecycle, role queries, and
// server-authoritative spawn/despawn; RPCs arrive in a later PR.
#pragma once

#include "fjell/api.h"
#include "fjell/asset.hpp"

#include <glm/glm.hpp>

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT int      fjell_net_role();                 // 0 none, 1 server, 2 client
FJELL_SDK_EXPORT uint32_t fjell_net_local_peer();
FJELL_SDK_EXPORT int      fjell_net_launch_mode();          // 0 offline, 1 host, 2 join
FJELL_SDK_EXPORT const char* fjell_net_join_address();
FJELL_SDK_EXPORT int      fjell_net_is_local_authority(uint32_t node_id);
FJELL_SDK_EXPORT int      fjell_net_is_owned_by_local(uint32_t node_id);
FJELL_SDK_EXPORT void     fjell_net_host(uint16_t port, uint32_t max_clients);
FJELL_SDK_EXPORT void     fjell_net_join(const char* address, uint16_t port);
FJELL_SDK_EXPORT void     fjell_net_disconnect();
FJELL_SDK_EXPORT uint32_t fjell_net_spawn(uint32_t preset_handle_id,
                                          float x, float y, float z, uint32_t owner_peer);
FJELL_SDK_EXPORT void     fjell_net_destroy(uint32_t net_id);
FJELL_SDK_EXPORT void     fjell_net_set_owner(uint32_t net_id, uint32_t peer);
FJELL_SDK_EXPORT void     fjell_net_send_rpc(uint32_t node_id, uint16_t rpc_index,
                                             const uint8_t* payload, uint32_t size);
}

namespace fjell {

struct Preset;

/// This peer's role in the current session.
enum class NetRole : int {
    none = 0,    ///< Single-player — not hosting or connected.
    server = 1,  ///< Authoritative host. Runs game logic for everyone.
    client = 2,  ///< Connected to a server; predicts locally.
};

/// What the editor's Net dropdown is set to for this run.
enum class NetLaunchMode : int {
    offline = 0,  ///< Start single-player.
    host = 1,     ///< Start a server.
    join = 2,     ///< Connect to `net::join_address()`.
};

/// Identifies one connected peer. Assigned by the server on connect.
using PeerId = uint32_t;

/// The server's own peer ID. Also the default owner for objects the server
/// spawns and keeps authority over.
constexpr PeerId SERVER_PEER = 0;

/// Multiplayer session control: connection lifecycle, authority queries, and
/// server-authoritative spawning.
///
/// The model is server-authoritative — the server owns game state, clients
/// send input and receive updates. Guard state-changing logic with
/// `is_server()`, and possession logic (camera, input) with
/// `is_owned_by_local()`.
///
/// @code
/// void begin_play() override {
///     switch (fjell::net::launch_mode()) {
///         case fjell::NetLaunchMode::host: fjell::net::host(7777); break;
///         case fjell::NetLaunchMode::join:
///             fjell::net::join(fjell::net::join_address(), 7777); break;
///         default: break;
///     }
/// }
/// @endcode
namespace net {
/// This peer's role, or `none` in single-player.
[[nodiscard]] inline NetRole role()        { return static_cast<NetRole>(fjell_net_role()); }

/// Whether this peer is the authoritative server. Gate damage, scoring,
/// and spawning on this.
[[nodiscard]] inline bool    is_server()   { return role() == NetRole::server; }

/// Whether this peer is a connected client.
[[nodiscard]] inline bool    is_client()   { return role() == NetRole::client; }

/// Whether a session is running at all. False in single-player, where
/// `is_server()` is also false — check this before assuming otherwise.
[[nodiscard]] inline bool    is_networked(){ return role() != NetRole::none; }

/// This peer's ID. Equals `SERVER_PEER` on the server.
[[nodiscard]] inline PeerId  local_peer()  { return fjell_net_local_peer(); }

/// What the editor's Net dropdown is set to for this run. Read it in
/// `begin_play()` to decide whether to `host()`, `join()`, or stay offline.
[[nodiscard]] inline NetLaunchMode launch_mode() {
    return static_cast<NetLaunchMode>(fjell_net_launch_mode());
}
/// Server address configured for Join mode in the editor.
[[nodiscard]] inline const char* join_address() { return fjell_net_join_address(); }

/// Whether this peer may change the node's authoritative state. True on
/// the server for every node, and on a client only for nodes it owns.
/// @param node_id Node to test.
/// @note This is about who simulates, not who plays. For "is this MY
///       player" use `is_owned_by_local()`.
[[nodiscard]] inline bool is_local_authority(uint32_t node_id) {
    return fjell_net_is_local_authority(node_id) != 0;
}

/// Whether the local player owns this node — the check for possession, so
/// only one player's pawn takes input and drives the camera.
/// @param node_id Node to test.
/// @note Differs from `is_local_authority()`, which is additionally true
///       on the server for every node it simulates but does not play.
[[nodiscard]] inline bool is_owned_by_local(uint32_t node_id) {
    return fjell_net_is_owned_by_local(node_id) != 0;
}

/// Start listening as the server.
/// @param port        UDP port to bind.
/// @param max_clients Maximum simultaneous connections.
inline void host(uint16_t port, uint32_t max_clients = 16) {
    fjell_net_host(port, max_clients);
}

/// Connect to a server as a client. Connection completes asynchronously —
/// `role()` stays `none` until it succeeds.
/// @param address Server hostname or IP.
/// @param port    Server's port.
inline void join(const char* address, uint16_t port) { fjell_net_join(address, port); }

/// Leave the session. On the server this disconnects every client.
inline void disconnect()                              { fjell_net_disconnect(); }

/// Spawn a preset on every peer, replicated and kept in sync. This is the
/// networked counterpart to `scene::spawn()`.
/// @param preset Preset asset, normally a `[[fj::property]] Asset<Preset>`.
/// @param pos    World position for the spawned root.
/// @param owner  Peer given ownership — pass the peer from
///               `on_player_connected()` for a player pawn. Defaults to
///               the server keeping it.
/// @return The new network ID, or 0 if called off the server or the preset
///         is unassigned.
/// @note Server only. Calling it on a client does nothing.
inline uint32_t spawn(const Asset<Preset>& preset, const glm::vec3& pos,
                      PeerId owner = SERVER_PEER) {
    if (!preset.valid()) return 0;
    return fjell_net_spawn(preset.handle_id(), pos.x, pos.y, pos.z, owner);
}
/// Despawn a replicated object on every peer. Server only.
/// @param net_id Network ID from `spawn()` — not a node ID.
inline void destroy(uint32_t net_id)             { fjell_net_destroy(net_id); }

/// Transfer ownership of a replicated object to another peer, handing over
/// input authority. Server only.
/// @param net_id Network ID from `spawn()`.
/// @param p      The peer taking ownership.
inline void set_owner(uint32_t net_id, PeerId p) { fjell_net_set_owner(net_id, p); }

/// @internal Called by generated `[[fj::rpc]]` send-wrappers. Declare an
/// RPC method instead of calling this directly.
inline void send_rpc(uint32_t node_id, uint16_t rpc_index,
                     const uint8_t* payload, uint32_t size) {
    fjell_net_send_rpc(node_id, rpc_index, payload, size);
}
} // namespace net

} // namespace fjell
