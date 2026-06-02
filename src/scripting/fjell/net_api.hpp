// net_api.hpp — Static networking API for game scripts.
//
// Mirrors SceneAPI / InputAPI: the engine exports fjell_net_* symbols
// (resolved at dlopen via -rdynamic), and this header wraps them in a
// convenient static struct. PR 1 covers connection lifecycle + role queries;
// spawn/RPC arrive in later PRs.
#pragma once

#include "fjell/api.h"

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT int      fjell_net_role();                 // 0 none, 1 server, 2 client
FJELL_SDK_EXPORT uint32_t fjell_net_local_peer();
FJELL_SDK_EXPORT int      fjell_net_is_local_authority(uint32_t node_id);
FJELL_SDK_EXPORT void     fjell_net_host(uint16_t port, uint32_t max_clients);
FJELL_SDK_EXPORT void     fjell_net_join(const char* address, uint16_t port);
FJELL_SDK_EXPORT void     fjell_net_disconnect();
}

namespace fjell {

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
};

} // namespace fjell
