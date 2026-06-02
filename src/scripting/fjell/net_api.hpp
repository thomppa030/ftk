// net_api.hpp — Static networking API for game scripts.
//
// Mirrors SceneAPI / InputAPI: the engine exports fjell_net_* symbols
// (resolved at dlopen via -rdynamic), and this header wraps them in a
// convenient static struct. Covers connection lifecycle, role queries, and
// server-authoritative spawn/despawn; RPCs arrive in a later PR.
#pragma once

#include "fjell/api.h"
#include "fjell/ref.hpp"

#include <glm/glm.hpp>

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT int      fjell_net_role();                 // 0 none, 1 server, 2 client
FJELL_SDK_EXPORT uint32_t fjell_net_local_peer();
FJELL_SDK_EXPORT int      fjell_net_is_local_authority(uint32_t node_id);
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

struct PresetAsset;

// Mirrors the engine's NetRole. Kept here so scripts get it without pulling in
// engine-internal headers.
enum class NetRole : int { none = 0, server = 1, client = 2 };

using PeerId = uint32_t;
constexpr PeerId SERVER_PEER = 0;

struct NetAPI {
    [[nodiscard]] static NetRole role()        { return static_cast<NetRole>(fjell_net_role()); }
    [[nodiscard]] static bool    is_server()   { return role() == NetRole::server; }
    [[nodiscard]] static bool    is_client()   { return role() == NetRole::client; }
    [[nodiscard]] static bool    is_networked(){ return role() != NetRole::none; }
    [[nodiscard]] static PeerId  local_peer()  { return fjell_net_local_peer(); }

    [[nodiscard]] static bool is_local_authority(uint32_t node_id) {
        return fjell_net_is_local_authority(node_id) != 0;
    }

    static void host(uint16_t port, uint32_t max_clients = 16) {
        fjell_net_host(port, max_clients);
    }
    static void join(const char* address, uint16_t port) { fjell_net_join(address, port); }
    static void disconnect()                              { fjell_net_disconnect(); }

    // Server-only: instantiate a preset on every peer. Returns the new NetId
    // (0 on failure or off the server). Hand authority to `owner` (default: the
    // server keeps it).
    static uint32_t spawn(const Ref<PresetAsset>& preset, const glm::vec3& pos,
                          PeerId owner = SERVER_PEER) {
        if (!preset.valid()) return 0;
        return fjell_net_spawn(preset.handle_id(), pos.x, pos.y, pos.z, owner);
    }
    static void destroy(uint32_t net_id)             { fjell_net_destroy(net_id); }
    static void set_owner(uint32_t net_id, PeerId p) { fjell_net_set_owner(net_id, p); }

    // Called by generated RPC send-wrappers — not meant for direct script use.
    static void send_rpc(uint32_t node_id, uint16_t rpc_index,
                         const uint8_t* payload, uint32_t size) {
        fjell_net_send_rpc(node_id, rpc_index, payload, size);
    }
};

} // namespace fjell
