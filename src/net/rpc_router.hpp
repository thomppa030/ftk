// rpc_router.hpp — Routes [[fj::rpc]] calls across the network by target.
//
// Stateless: it resolves the calling/target component, builds the rpc_call
// packet via the codec, and sends/broadcasts/invokes per the RPC's target
// (server / clients / all). Collaborators (Scene, ScriptManager, NetScene,
// Transport, role) are passed in per call.
#pragma once

#include "net/net_types.hpp"

#include <cstdint>
#include <span>

namespace fjell {

class Scene;
class ScriptManager;
class NetScene;
class Transport;

class RpcRouter {
public:
    // Called by the generated send-wrapper. Routes an RPC_CALL by target:
    // server-target → to the host; clients-target → broadcast to clients;
    // all → run locally + forward. node_id is the calling component's node.
    void send(Scene& scene, ScriptManager& scripts, const NetScene& net_scene,
              Transport* transport, NetRole role, uint32_t node_id,
              uint16_t rpc_index, const uint8_t* payload, uint32_t size);

    // Apply a received rpc_call packet: invoke the handler on the target node,
    // and (on the server) re-broadcast "all" RPCs to clients.
    void apply(Scene& scene, ScriptManager& scripts, const NetScene& net_scene,
               Transport* transport, NetRole role, std::span<const uint8_t> data);
};

} // namespace fjell
