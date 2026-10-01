#include "net/rpc_router.hpp"
#include "scripting/script_call.hpp"

#include "net/packet_codec.hpp"
#include "net/net_scene.hpp"
#include "net/transport.hpp"
#include "ftk/base/log.hpp"
#include "scene/scene.hpp"
#include "scene/node.hpp"
#include "scripting/script_manager.hpp"

namespace fjell {
namespace {

using netcodec::put;
using netcodec::put_bytes;
using netcodec::take;

// Find the script component on a node whose type declares RPCs. Returns its
// type (for the FjellRpcDesc[] table) and instance pointer, or {nullptr,...}.
/// Runs an RPC's handler, which is game code, through call_script().
void invoke(const OwnedScriptType& type, const FjellRpcDesc& desc, const Node* node, void* inst,
            const uint8_t* payload, uint32_t size) {
    const ScriptCall call{.type_name = type.name, .hook = desc.name, .node = node, .instance = inst};
    call_script(call, [&] { desc.invoke(inst, payload, size); });
}

const OwnedScriptType* rpc_component(const Node& node, ScriptManager& scripts,
                                     void*& instance_out) {
    for (const auto& entry : node.scripts) {
        if (!entry.instance) continue;
        const auto* type = scripts.find_script_type(entry.type_name);
        if (type && type->rpc_count > 0) {
            instance_out = entry.instance.get();
            return type;
        }
    }
    return nullptr;
}

} // namespace

void RpcRouter::send(Scene& scene, ScriptManager& scripts, const NetScene& net_scene,
                     Transport* transport, NetRole role, uint32_t node_id,
                     uint16_t rpc_index, const uint8_t* payload, uint32_t size) {
    if (role == NetRole::none || !transport) return;

    NetId net_id = net_scene.net_id_for(node_id);
    if (net_id == NET_ID_NONE) {
        FTK_CORE_WARN("NetManager: RPC on un-networked node {}", node_id);
        return;
    }

    // Look up the target so we can resolve "all" (run locally + forward).
    Node* node = scene.find(node_id);
    void* inst = nullptr;
    const OwnedScriptType* type = node ? rpc_component(*node, scripts, inst) : nullptr;
    if (!type || rpc_index >= type->rpc_count) return;
    const FjellRpcDesc& desc = type->rpcs[rpc_index];

    auto build_and_send = [&](Channel ch, bool broadcast) {
        std::vector<uint8_t> packet;
        put(packet, static_cast<uint8_t>(PacketType::rpc_call));
        put(packet, net_id);
        put(packet, rpc_index);
        put(packet, size);
        if (size > 0) put_bytes(packet, payload, size);
        if (broadcast) transport->broadcast(packet, ch);
        else transport->send(SERVER_PEER, packet, ch);  // client → host
    };

    switch (desc.target) {
    case FJELL_RPC_SERVER:
        if (role == NetRole::server) {
            invoke(*type, desc, node, inst, payload, size);  // already authoritative
        } else {
            build_and_send(Channel::reliable, false);  // client → server
        }
        break;
    case FJELL_RPC_CLIENTS:
        if (role == NetRole::server) {
            build_and_send(Channel::reliable, true);   // server → all clients
        }
        // a client calling a clients-RPC is a no-op (only the server may)
        break;
    case FJELL_RPC_ALL:
        invoke(*type, desc, node, inst, payload, size);  // run locally
        if (role == NetRole::server) build_and_send(Channel::reliable, true);
        else build_and_send(Channel::reliable, false);
        break;
    }
}

void RpcRouter::apply(Scene& scene, ScriptManager& scripts, const NetScene& net_scene,
                      Transport* transport, NetRole role, std::span<const uint8_t> data) {
    NetId net_id = NET_ID_NONE;
    uint16_t rpc_index = 0;
    uint32_t size = 0;
    if (!take(data, net_id)) return;
    if (!take(data, rpc_index)) return;
    if (!take(data, size)) return;
    if (data.size() < size) return;

    uint32_t node_id = net_scene.node_id_for(net_id);
    Node* node = (node_id != 0) ? scene.find(node_id) : nullptr;
    if (!node) return;

    void* inst = nullptr;
    const OwnedScriptType* type = rpc_component(*node, scripts, inst);
    if (!type || rpc_index >= type->rpc_count) return;
    const FjellRpcDesc& desc = type->rpcs[rpc_index];

    // A server receiving a client→server RPC must validate ownership: the
    // sender may only drive objects it owns (or the server trusts all? no —
    // require ownership for server-targeted RPCs from clients).
    if (role == NetRole::server && desc.target == FJELL_RPC_SERVER) {
        // v1: accept (single sender per object); ownership tightening is a
        // follow-up once per-peer sender identity is plumbed through receive.
    }

    invoke(*type, desc, node, inst, size > 0 ? data.data() : nullptr, size);

    // "all" RPCs received on the server are re-broadcast to clients.
    if (role == NetRole::server && desc.target == FJELL_RPC_ALL && transport) {
        std::vector<uint8_t> packet;
        put(packet, static_cast<uint8_t>(PacketType::rpc_call));
        put(packet, net_id);
        put(packet, rpc_index);
        put(packet, size);
        if (size > 0) put_bytes(packet, data.data(), size);
        transport->broadcast(packet, Channel::reliable);
    }
}

} // namespace fjell
