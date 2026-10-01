#include "net/replication_graph.hpp"

#include "net/packet_codec.hpp"
#include "net/net_scene.hpp"
#include "net/prediction_state.hpp"
#include "net/transport.hpp"
#include "ftk/base/log.hpp"
#include "scene/scene.hpp"
#include "scene/node.hpp"
#include "scene/component_registry.hpp"
#include "physics/physics_types.hpp"
#include "scripting/script_manager.hpp"

#include <algorithm>
#include <cstring>

namespace fjell {
namespace {

using netcodec::put;
using netcodec::put_bytes;
using netcodec::take;

// Collect the first script component on the node that has replicated fields,
// along with its replicated-field descriptor list. One networked component per
// node for v1 (the common case: a Player/Enemy controller).
std::vector<const OwnedFieldDesc*> replicated_descs(const Node& node,
                                                    ScriptManager& scripts,
                                                    const uint8_t*& instance_out) {
    std::vector<const OwnedFieldDesc*> out;
    for (const auto& entry : node.scripts) {
        if (!entry.instance) continue;
        const auto* type = scripts.find_script_type(entry.type_name);
        if (!type) continue;
        for (const auto& f : type->fields) {
            if (f.replicated) out.push_back(&f);
        }
        if (!out.empty()) {
            instance_out = reinterpret_cast<const uint8_t*>(entry.instance.get());
            return out;
        }
    }
    return out;
}

// Stable 32-bit key for a replicated engine component, derived from its
// ComponentMeta::json_key so sender and receiver agree without sharing a numeric
// id (FNV-1a; the same key string always hashes identically across builds).
uint32_t component_net_key(std::string_view json_key) {
    uint32_t h = 2166136261u;
    for (char c : json_key) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    return h;
}

// Serialize every replicated engine component on a node into the packet as a
// section: comp_count(u8) then per component { key_hash(u32), size(u16), bytes }.
// Mirrors how replicated script fields are packed; the bytes are opaque to the
// netcode (only each component's net_serialize knows the layout).
void put_component_section(std::vector<uint8_t>& packet, Node& node) {
    const size_t count_pos = packet.size();
    packet.push_back(0);  // comp_count placeholder
    uint8_t comp_count = 0;

    node.components.for_each([&](ComponentTypeId id, void* ptr) {
        const auto* meta = ComponentRegistry::instance().find(id);
        if (!meta || !meta->net_serialize) return;

        std::vector<uint8_t> bytes;
        meta->net_serialize(ptr, bytes);
        if (bytes.empty()) return;  // nothing to sync (e.g. animator with no params)

        put(packet, component_net_key(meta->json_key));
        put(packet, static_cast<uint16_t>(bytes.size()));
        put_bytes(packet, bytes.data(), bytes.size());
        ++comp_count;
    });

    packet[count_pos] = comp_count;
}

// Advance `data` past a component section without applying it (the object wasn't
// accepted from this sender, but its bytes still need consuming so the rest of
// the packet parses). Returns false on truncation.
[[nodiscard]] bool skip_component_section(std::span<const uint8_t>& data) {
    uint8_t comp_count = 0;
    if (!take(data, comp_count)) return false;
    for (uint8_t i = 0; i < comp_count; ++i) {
        uint32_t key = 0;
        uint16_t size = 0;
        if (!take(data, key)) return false;
        if (!take(data, size)) return false;
        if (data.size() < size) return false;
        data = data.subspan(size);
    }
    return true;
}

// Read a component section written by put_component_section and dispatch each
// block to the matching component's net_deserialize (looked up by key hash).
// Advances `data` past the whole section. Returns false on truncation.
[[nodiscard]] bool apply_component_section(std::span<const uint8_t>& data, Node& node) {
    uint8_t comp_count = 0;
    if (!take(data, comp_count)) return false;

    for (uint8_t i = 0; i < comp_count; ++i) {
        uint32_t key_hash = 0;
        uint16_t size = 0;
        if (!take(data, key_hash)) return false;
        if (!take(data, size)) return false;
        if (data.size() < size) return false;

        auto block = data.subspan(0, size);
        data = data.subspan(size);

        // Find the component on this node whose meta key matches the hash.
        node.components.for_each([&](ComponentTypeId id, void* ptr) {
            const auto* meta = ComponentRegistry::instance().find(id);
            if (meta && meta->net_deserialize &&
                component_net_key(meta->json_key) == key_hash) {
                meta->net_deserialize(ptr, block);
            }
        });
    }
    return true;
}

} // namespace

void ReplicationGraph::ensure_scene_registered(Scene& scene, ScriptManager& scripts,
                                               NetScene& net_scene) {
    if (scene_registered_) return;
    scene_registered_ = true;

    // Collect node ids that carry a replicated component, sorted for a stable
    // NetId assignment that both peers compute identically.
    std::vector<uint32_t> ids;
    for (const auto& node : scene.objects()) {
        const uint8_t* inst = nullptr;
        if (!replicated_descs(*node, scripts, inst).empty()) {
            ids.push_back(node->id);
        }
    }
    std::sort(ids.begin(), ids.end());
    for (uint32_t id : ids) {
        net_scene.register_object(net_scene.assign_net_id(), id);
    }
    if (!ids.empty()) {
        FJELL_CORE_INFO("NetManager: registered {} networked object(s)", ids.size());
    }
}

void ReplicationGraph::collect_and_send(Scene& scene, ScriptManager& scripts,
                                        NetScene& net_scene, Transport* transport,
                                        NetRole role, PeerId local_peer) {
    if (!transport) return;
    ensure_scene_registered(scene, scripts, net_scene);
    if (role == NetRole::none) { transport->flush(); return; }

    // Each peer authors only the objects it has authority over:
    //   server → every object EXCEPT those a client owns (the client authors those)
    //   client → only the objects it owns (its own player)
    // This prevents the server's stale copy of a client-owned object from fighting
    // that client's prediction (which yanked it back to spawn).
    const bool is_server = (role == NetRole::server);
    auto authored_here = [&](NetId net_id) {
        PeerId owner = net_scene.owner_of(net_id);
        return is_server ? (owner == SERVER_PEER)
                         : net_scene.is_owned_by_local(net_id, role, local_peer);
    };

    ++server_tick_;

    std::vector<uint8_t> packet;
    put(packet, static_cast<uint8_t>(PacketType::state_update));
    put(packet, server_tick_);
    const size_t count_pos = packet.size();
    put(packet, static_cast<uint16_t>(0));  // object_count placeholder

    uint16_t object_count = 0;

    for (const auto& [net_id, node_id] : net_scene.objects()) {
        if (!authored_here(net_id)) continue;  // not ours to author
        Node* node = scene.find(node_id);
        if (!node) continue;

        const uint8_t* instance = nullptr;
        auto descs = replicated_descs(*node, scripts, instance);

        // Build this object's current replicated bytes (fields, then transform,
        // then the engine-component section). Concatenated into one buffer so the
        // single per-object shadow catches a change in any of the three.
        std::vector<uint8_t> current;
        for (const auto* f : descs) {
            put_bytes(current, instance + f->offset, f->size);
        }
        Node3D* spatial = node->as_spatial();
        const bool has_transform = spatial != nullptr;
        if (has_transform) {
            put(current, spatial->transform.position);
            put(current, spatial->transform.rotation);
        }
        const size_t comp_off = current.size();
        put_component_section(current, *node);  // self-delimiting (comp_count + blocks)
        if (current.empty()) continue;  // nothing replicated on this object

        // Diff against the shadow to find which fields changed.
        auto& shadow = shadow_[net_id].data;
        const bool first_send = shadow.size() != current.size();

        // dirty_mask: one bit per replicated field (transform handled separately).
        uint16_t dirty_mask = 0;
        size_t off = 0;
        for (size_t i = 0; i < descs.size(); ++i) {
            const uint32_t sz = descs[i]->size;
            const bool changed = first_send ||
                std::memcmp(current.data() + off, shadow.data() + off, sz) != 0;
            if (changed) dirty_mask |= static_cast<uint16_t>(1u << i);
            off += sz;
        }
        const size_t transform_len = comp_off - off;  // transform bytes between fields and comps
        bool transform_changed = has_transform;
        if (has_transform && !first_send) {
            transform_changed =
                std::memcmp(current.data() + off, shadow.data() + off,
                            transform_len) != 0;
        }
        const bool comp_changed = first_send ||
            shadow.size() != current.size() ||
            std::memcmp(current.data() + comp_off, shadow.data() + comp_off,
                        current.size() - comp_off) != 0;

        if (dirty_mask == 0 && !transform_changed && !comp_changed) continue;  // unchanged

        put(packet, net_id);
        put(packet, static_cast<uint8_t>(has_transform ? 1 : 0));
        if (has_transform) {
            put(packet, spatial->transform.position);
            put(packet, spatial->transform.rotation);
        }
        put(packet, dirty_mask);
        off = 0;
        for (size_t i = 0; i < descs.size(); ++i) {
            const uint32_t sz = descs[i]->size;
            if (dirty_mask & (1u << i)) {
                put_bytes(packet, current.data() + off, sz);
            }
            off += sz;
        }
        // Component section: always sent in full (it's small) so the receiver can
        // re-drive each component's state regardless of which field changed.
        put_bytes(packet, current.data() + comp_off, current.size() - comp_off);

        shadow = std::move(current);
        ++object_count;
    }

    if (object_count > 0) {
        std::memcpy(packet.data() + count_pos, &object_count, sizeof(object_count));
        if (is_server) transport->broadcast(packet, Channel::unreliable);
        else transport->send(SERVER_PEER, packet, Channel::unreliable);  // client → server
    }
    transport->flush();
}

void ReplicationGraph::apply_state_update(Scene& scene, ScriptManager& scripts,
                                          NetScene& net_scene, PredictionState& prediction,
                                          Transport* transport, NetRole role,
                                          PeerId sender, PeerId local_peer,
                                          std::span<const uint8_t> data) {
    uint32_t tick = 0;
    uint16_t object_count = 0;
    if (!take(data, tick)) return;
    if (!take(data, object_count)) return;

    // The server relays client-authored updates to the other clients. Rebuild a
    // forward packet as we go (only the objects we accept from this sender).
    const bool relay = (role == NetRole::server);
    std::vector<uint8_t> fwd;
    size_t fwd_count_pos = 0;
    uint16_t fwd_count = 0;
    if (relay) {
        put(fwd, static_cast<uint8_t>(PacketType::state_update));
        put(fwd, tick);
        fwd_count_pos = fwd.size();
        put(fwd, static_cast<uint16_t>(0));
    }

    for (uint16_t o = 0; o < object_count; ++o) {
        NetId net_id = NET_ID_NONE;
        uint8_t has_transform = 0;
        if (!take(data, net_id)) return;
        if (!take(data, has_transform)) return;

        glm::vec3 position{};
        glm::quat rotation{};
        if (has_transform) {
            if (!take(data, position)) return;
            if (!take(data, rotation)) return;
        }

        uint16_t dirty_mask = 0;
        if (!take(data, dirty_mask)) return;

        // Decide whether to accept this object's state from THIS sender:
        //   server: accept only if the sender owns the object (client authority);
        //   client: accept anything it doesn't own itself (the server sent it).
        bool accept;
        bool owned_local;  // for the predict-vs-interpolate branch below
        if (role == NetRole::server) {
            accept = (net_scene.owner_of(net_id) == sender) && (sender != SERVER_PEER);
            owned_local = false;  // server snaps client-owned objects directly
        } else {
            owned_local = net_scene.is_owned_by_local(net_id, role, local_peer);
            accept = !owned_local;
        }

        uint32_t node_id = net_scene.node_id_for(net_id);
        Node* node = (node_id != 0) ? scene.find(node_id) : nullptr;

        const uint8_t* cinst = nullptr;
        auto descs = node ? replicated_descs(*node, scripts, cinst)
                          : std::vector<const OwnedFieldDesc*>{};

        // Capture the slice of field bytes for relay, and apply if accepted.
        const uint8_t* fields_begin = data.data();
        for (size_t i = 0; i < descs.size(); ++i) {
            if (!(dirty_mask & (1u << i))) continue;
            const uint32_t sz = descs[i]->size;
            if (data.size() < sz) return;
            if (accept && cinst) {
                auto* dst = const_cast<uint8_t*>(cinst) + descs[i]->offset;
                std::memcpy(dst, data.data(), sz);
            }
            data = data.subspan(sz);
        }

        // Engine-component section follows the field bytes. Apply it to the node
        // when accepted; otherwise just skip past it so the next object parses
        // correctly. Either way `data` advances over the whole section, and the
        // combined field+component slice is captured for relay.
        const uint8_t* payload_begin = fields_begin;  // fields + component section
        if (accept && node) {
            if (!apply_component_section(data, *node)) return;
        } else if (!skip_component_section(data)) {
            return;
        }
        const size_t payload_len = static_cast<size_t>(data.data() - payload_begin);

        if (accept && has_transform && node && node->as_spatial()) {
            if (owned_local) {
                // (client only) reconcile our prediction toward the authority.
                prediction.reconcile_owned(net_id, *node->as_spatial(), position, rotation);
            } else {
                // Remote object we don't own — smooth between 30 Hz snapshots
                // instead of snapping. Runs on the client (host's player) AND on
                // the server (each client's player), so motion looks identical in
                // every window. interpolate_remotes drives the actual transform.
                prediction.push_sample(net_id, position, rotation);
                // The peer that owns this character simulates it; here it only
                // has to be where the snapshots say, with its capsule along.
                if (auto* character = node->components.get<CharacterComponent>()) {
                    character->simulate = false;
                }
            }
        }

        // Append accepted objects to the relay packet (field bytes + component
        // section forwarded verbatim).
        if (relay && accept) {
            put(fwd, net_id);
            put(fwd, has_transform);
            if (has_transform) { put(fwd, position); put(fwd, rotation); }
            put(fwd, dirty_mask);
            if (payload_len > 0) put_bytes(fwd, payload_begin, payload_len);
            ++fwd_count;
        }
    }

    // Relay accepted client updates to all clients (the sender's own copy is its
    // prediction; harmless to echo, and other clients need it).
    if (relay && fwd_count > 0 && transport) {
        std::memcpy(fwd.data() + fwd_count_pos, &fwd_count, sizeof(fwd_count));
        transport->broadcast(fwd, Channel::unreliable);
    }
}

} // namespace fjell
