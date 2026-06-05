// net_types.hpp — Small shared types for the networking layer.
//
// Lightweight definitions (NetId, NetRole, NetLaunchMode) that the net
// components (NetScene, NetManager, subsystems) all need, factored out so they
// can depend on the types without pulling in net_manager.hpp. PeerId / SERVER_PEER
// come from transport.hpp.
#pragma once

#include "net/transport.hpp"

#include <cstdint>

namespace fjell {

// Network identity, distinct from a scene node id. Assigned by the server and
// replicated so every peer agrees which object is which.
using NetId = uint32_t;
constexpr NetId NET_ID_NONE = 0;

enum class NetRole : uint8_t {
    none,    // not networked (single-player / edit mode)
    server,  // authoritative host
    client,  // connected to a host
};

// What the editor's Net dropdown is set to. The engine never acts on this — it's
// stored here so the running game script can read it (via NetAPI::launch_mode())
// in begin_play and decide whether to host(), join(), or stay offline. Keeping
// the policy in the script keeps the engine game-agnostic.
enum class NetLaunchMode : uint8_t {
    offline,  // single-player; script does nothing
    host,     // script should host()
    join,     // script should join() the configured address
};

} // namespace fjell
