#include "net/net_manager.hpp"

#include "net/packet_codec.hpp"
#include "net/enet_transport.hpp"
#include "core/log.hpp"
#include "scene/scene.hpp"
#include "scene/node.hpp"
#include "scene/preset_asset.hpp"
#include "scene/component_registry.hpp"
#include "scripting/script_manager.hpp"
#include "renderer/resources/resource_manager.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace fjell {

// Wire-format primitives now live in packet_codec.hpp; pull them into this TU's
// scope so the existing call sites (put/take/put_string/...) resolve unchanged.
using netcodec::put;
using netcodec::put_bytes;
using netcodec::take;
using netcodec::put_string;
using netcodec::take_string;

NetManager::NetManager() {
    // Seed the launch mode from FJELL_NET_MODE so the env-var workflow keeps
    // working for headless / scripted runs; the editor's Net dropdown overrides
    // it interactively before Play.
    if (const char* mode = std::getenv("FJELL_NET_MODE")) {
        if (std::strcmp(mode, "host") == 0) launch_mode_ = NetLaunchMode::host;
        else if (std::strcmp(mode, "join") == 0) launch_mode_ = NetLaunchMode::join;
    }
}
NetManager::~NetManager() { shutdown(); }

// ── Lifecycle ───────────────────────────────────────────────────────────

void NetManager::host(uint16_t port, uint32_t max_clients) {
    shutdown();

    auto transport = std::make_unique<EnetTransport>();
    transport->on_connect    = [this](PeerId p) { handle_connect(p); };
    transport->on_disconnect = [this](PeerId p) { handle_disconnect(p); };
    transport->on_receive    = [this](PeerId p, std::span<const uint8_t> d, Channel c) {
        if (rx_scene_ && rx_scripts_ && rx_resources_)
            handle_receive(*rx_scene_, *rx_scripts_, *rx_resources_, p, d, c);
    };

    if (!transport->host(port, max_clients)) {
        FJELL_CORE_ERROR("NetManager: failed to host on port {}", port);
        return;
    }

    transport_  = std::move(transport);
    role_       = NetRole::server;
    local_peer_ = SERVER_PEER;
    FJELL_CORE_INFO("NetManager: hosting (server peer {})", local_peer_);
}

void NetManager::join(const std::string& address, uint16_t port) {
    shutdown();

    auto transport = std::make_unique<EnetTransport>();
    transport->on_connect    = [this](PeerId p) { handle_connect(p); };
    transport->on_disconnect = [this](PeerId p) { handle_disconnect(p); };
    transport->on_receive    = [this](PeerId p, std::span<const uint8_t> d, Channel c) {
        if (rx_scene_ && rx_scripts_ && rx_resources_)
            handle_receive(*rx_scene_, *rx_scripts_, *rx_resources_, p, d, c);
    };

    if (!transport->connect(address, port)) {
        FJELL_CORE_ERROR("NetManager: failed to connect to {}:{}", address, port);
        return;
    }

    transport_  = std::move(transport);
    role_       = NetRole::client;
    // The client's own PeerId is assigned by the server post-handshake; until
    // then it is unknown. SERVER_PEER here just means "not yet a distinct peer".
    local_peer_ = SERVER_PEER;
    FJELL_CORE_INFO("NetManager: joining {}:{}", address, port);
}

void NetManager::shutdown() {
    if (transport_) {
        transport_->disconnect();
        transport_.reset();
    }
    role_ = NetRole::none;
    local_peer_ = SERVER_PEER;
    server_tick_ = 0;
    scene_registered_ = false;
    next_net_id_ = 1;
    net_to_node_.clear();
    node_to_net_.clear();
    ownership_.clear();
    shadow_.clear();
    interp_.clear();
    corrections_.clear();
    spawn_info_.clear();
    net_time_ = 0.0f;
    newest_sample_t_ = 0.0f;
    net_time_primed_ = false;
    rx_scene_ = nullptr;
    rx_scripts_ = nullptr;
    rx_resources_ = nullptr;
    rx_project_ = nullptr;
}

// ── Replication helpers ─────────────────────────────────────────────────────

namespace {

// Gather a script component's [[fj::replicated]] fields on a node into a flat
// byte buffer (field values concatenated in field order) plus the matching
// descriptors. Returns the descriptors so the caller can build a dirty mask.
struct ReplFields {
    const std::vector<OwnedFieldDesc>* descs{nullptr};  // replicated subset, in order
    const uint8_t* instance{nullptr};                    // component instance base
};

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

// Find the script component on a node whose type declares RPCs. Returns its
// type (for the FjellRpcDesc[] table) and instance pointer, or {nullptr,...}.
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

void NetManager::ensure_scene_registered(Scene& scene, ScriptManager& scripts) {
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
        register_net_object(assign_net_id(), id);
    }
    if (!ids.empty()) {
        FJELL_CORE_INFO("NetManager: registered {} networked object(s)", ids.size());
    }
}

// ── Per-frame pumps ───────────────────────────────────────────────────────

void NetManager::receive_and_apply(Scene& scene, ScriptManager& scripts,
                                   ResourceManager& resources, const Project* project) {
    if (!transport_) return;
    ensure_scene_registered(scene, scripts);
    rx_scene_ = &scene;
    rx_scripts_ = &scripts;
    rx_resources_ = &resources;
    rx_project_ = project;

    // Defer spawns applied during poll() (apply_spawn → instantiate → add_node):
    // adding directly would mutate objects_ mid-iteration, AND would bypass the
    // engine's flush_pending, which is what materializes spawns, runs their script
    // lifecycle (begin_play), and loads any InputComponent contexts they carry.
    // Holding the lock here makes those adds defer to the pending queue; the
    // engine's flush_pending (right after this phase-0 tick) does the rest — so a
    // network-spawned player gets the same begin_play + possession + input as a
    // locally-spawned one.
    Scene::TickLock lock(scene);
    transport_->poll(0.0f);
}

void NetManager::collect_and_send(Scene& scene, ScriptManager& scripts) {
    if (!transport_) return;
    ensure_scene_registered(scene, scripts);
    if (role_ == NetRole::none) { transport_->flush(); return; }

    // Each peer authors only the objects it has authority over:
    //   server → every object EXCEPT those a client owns (the client authors those)
    //   client → only the objects it owns (its own player)
    // This prevents the server's stale copy of a client-owned object from fighting
    // that client's prediction (which yanked it back to spawn).
    const bool is_server = (role_ == NetRole::server);
    auto authored_here = [&](NetId net_id) {
        PeerId owner = owner_of(net_id);
        return is_server ? (owner == SERVER_PEER) : is_owned_by_local(net_id);
    };

    ++server_tick_;

    std::vector<uint8_t> packet;
    put(packet, static_cast<uint8_t>(PacketType::state_update));
    put(packet, server_tick_);
    const size_t count_pos = packet.size();
    put(packet, static_cast<uint16_t>(0));  // object_count placeholder

    uint16_t object_count = 0;

    for (const auto& [net_id, node_id] : net_to_node_) {
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
        if (is_server) transport_->broadcast(packet, Channel::unreliable);
        else transport_->send(SERVER_PEER, packet, Channel::unreliable);  // client → server
    }
    transport_->flush();
}

// ── Network identity ──────────────────────────────────────────────────────

NetId NetManager::assign_net_id() {
    return next_net_id_++;
}

void NetManager::register_net_object(NetId net_id, uint32_t node_id) {
    net_to_node_[net_id] = node_id;
    node_to_net_[node_id] = net_id;
}

void NetManager::unregister_net_object(NetId net_id) {
    auto it = net_to_node_.find(net_id);
    if (it != net_to_node_.end()) {
        node_to_net_.erase(it->second);
        net_to_node_.erase(it);
    }
    ownership_.erase(net_id);
    shadow_.erase(net_id);
    interp_.erase(net_id);
    corrections_.erase(net_id);
    spawn_info_.erase(net_id);
}

// ── Spawn / despawn ─────────────────────────────────────────────────────────

std::vector<uint8_t> NetManager::build_spawn_packet(NetId net_id) const {
    std::vector<uint8_t> packet;
    auto it = spawn_info_.find(net_id);
    if (it == spawn_info_.end()) return packet;
    const SpawnInfo& info = it->second;
    put(packet, static_cast<uint8_t>(PacketType::spawn));
    put(packet, net_id);
    put(packet, info.owner);
    put_string(packet, info.preset_path);
    put(packet, info.position);
    return packet;
}

NetId NetManager::spawn(Scene& scene, ResourceManager& resources,
                        const std::string& preset_path, const glm::vec3& position,
                        PeerId owner, const Project* project) {
    if (role_ != NetRole::server) {
        FJELL_CORE_WARN("NetManager: spawn() called off the server, ignoring");
        return NET_ID_NONE;
    }

    auto handle = resources.load_preset_asset(preset_path, project);
    if (!handle) {
        FJELL_CORE_ERROR("NetManager: spawn failed, can't load preset '{}'", preset_path);
        return NET_ID_NONE;
    }
    PresetAsset* asset = resources.get(*handle);
    if (!asset) return NET_ID_NONE;

    Transform offset;
    offset.position = position;
    uint32_t node_id = asset->preset.instantiate(scene, -1, offset);
    if (node_id == 0) return NET_ID_NONE;

    NetId net_id = assign_net_id();
    register_net_object(net_id, node_id);
    set_owner(net_id, owner);
    spawn_info_[net_id] = {preset_path, owner, position};

    // Tell every connected client to instantiate the same preset.
    if (transport_) {
        auto packet = build_spawn_packet(net_id);
        transport_->broadcast(packet, Channel::reliable);
    }

    FJELL_CORE_INFO("NetManager: spawned '{}' as net {} (owner {})",
                    preset_path, net_id, owner);
    return net_id;
}

void NetManager::destroy(Scene& scene, NetId net_id) {
    if (role_ != NetRole::server) {
        FJELL_CORE_WARN("NetManager: destroy() called off the server, ignoring");
        return;
    }

    uint32_t node_id = node_id_for(net_id);
    if (node_id != 0) scene.queue_destroy(node_id);
    unregister_net_object(net_id);

    std::vector<uint8_t> packet;
    put(packet, static_cast<uint8_t>(PacketType::despawn));
    put(packet, net_id);
    if (transport_) transport_->broadcast(packet, Channel::reliable);

    FJELL_CORE_INFO("NetManager: despawned net {}", net_id);
}

uint32_t NetManager::node_id_for(NetId net_id) const {
    auto it = net_to_node_.find(net_id);
    return it != net_to_node_.end() ? it->second : 0;
}

NetId NetManager::net_id_for(uint32_t node_id) const {
    auto it = node_to_net_.find(node_id);
    return it != node_to_net_.end() ? it->second : NET_ID_NONE;
}

// ── RPC ─────────────────────────────────────────────────────────────────────

void NetManager::send_rpc(Scene& scene, ScriptManager& scripts, uint32_t node_id,
                          uint16_t rpc_index, const uint8_t* payload, uint32_t size) {
    if (role_ == NetRole::none || !transport_) return;

    NetId net_id = net_id_for(node_id);
    if (net_id == NET_ID_NONE) {
        FJELL_CORE_WARN("NetManager: RPC on un-networked node {}", node_id);
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
        if (broadcast) transport_->broadcast(packet, ch);
        else transport_->send(SERVER_PEER, packet, ch);  // client → host
    };

    switch (desc.target) {
    case FJELL_RPC_SERVER:
        if (role_ == NetRole::server) {
            desc.invoke(inst, payload, size);          // already authoritative
        } else {
            build_and_send(Channel::reliable, false);  // client → server
        }
        break;
    case FJELL_RPC_CLIENTS:
        if (role_ == NetRole::server) {
            build_and_send(Channel::reliable, true);   // server → all clients
        }
        // a client calling a clients-RPC is a no-op (only the server may)
        break;
    case FJELL_RPC_ALL:
        desc.invoke(inst, payload, size);              // run locally
        if (role_ == NetRole::server) build_and_send(Channel::reliable, true);
        else build_and_send(Channel::reliable, false);
        break;
    }
}

void NetManager::apply_rpc(Scene& scene, ScriptManager& scripts,
                           std::span<const uint8_t> data) {
    NetId net_id = NET_ID_NONE;
    uint16_t rpc_index = 0;
    uint32_t size = 0;
    if (!take(data, net_id)) return;
    if (!take(data, rpc_index)) return;
    if (!take(data, size)) return;
    if (data.size() < size) return;

    uint32_t node_id = node_id_for(net_id);
    Node* node = (node_id != 0) ? scene.find(node_id) : nullptr;
    if (!node) return;

    void* inst = nullptr;
    const OwnedScriptType* type = rpc_component(*node, scripts, inst);
    if (!type || rpc_index >= type->rpc_count) return;
    const FjellRpcDesc& desc = type->rpcs[rpc_index];

    // A server receiving a client→server RPC must validate ownership: the
    // sender may only drive objects it owns (or the server trusts all? no —
    // require ownership for server-targeted RPCs from clients).
    if (role_ == NetRole::server && desc.target == FJELL_RPC_SERVER) {
        // v1: accept (single sender per object); ownership tightening is a
        // follow-up once per-peer sender identity is plumbed through receive.
    }

    desc.invoke(inst, size > 0 ? data.data() : nullptr, size);

    // "all" RPCs received on the server are re-broadcast to clients.
    if (role_ == NetRole::server && desc.target == FJELL_RPC_ALL && transport_) {
        std::vector<uint8_t> packet;
        put(packet, static_cast<uint8_t>(PacketType::rpc_call));
        put(packet, net_id);
        put(packet, rpc_index);
        put(packet, size);
        if (size > 0) put_bytes(packet, data.data(), size);
        transport_->broadcast(packet, Channel::reliable);
    }
}

// ── Ownership ─────────────────────────────────────────────────────────────

void NetManager::set_owner(NetId net_id, PeerId peer) {
    ownership_[net_id] = peer;
}

PeerId NetManager::owner_of(NetId net_id) const {
    auto it = ownership_.find(net_id);
    return it != ownership_.end() ? it->second : SERVER_PEER;
}

bool NetManager::is_local_authority(NetId net_id) const {
    // The server is authoritative over everything. A client has authority only
    // over objects EXPLICITLY owned by its peer; everything else (the default)
    // belongs to the server, so the client applies the server's state to it.
    // Note local_peer_ is unreliable until peer-id assignment (PR 6), so we key
    // strictly off an explicit ownership entry, not the default.
    if (role_ == NetRole::server) return true;
    if (role_ == NetRole::client) {
        auto it = ownership_.find(net_id);
        return it != ownership_.end() && it->second == local_peer_ &&
               local_peer_ != SERVER_PEER;
    }
    return true;  // single-player: always authoritative
}

bool NetManager::is_owned_by_local(NetId net_id) const {
    // Possession is about the local player, not server authority: the object's
    // owner must equal this peer's id. On the host, local_peer_ == SERVER_PEER, so
    // the host possesses objects it spawned for itself (owner 0) but NOT a client's
    // player (owner 1). Offline (role none) everything is "owned" by the player.
    if (role_ == NetRole::none) return true;
    return owner_of(net_id) == local_peer_;
}

// ── Transport callbacks ─────────────────────────────────────────────────────

void NetManager::handle_connect(PeerId peer) {
    if (role_ == NetRole::server && transport_) {
        // WELCOME first (reliable, ordered): the client learns its peer id before
        // any spawn arrives, so ownership/authority resolves correctly.
        std::vector<uint8_t> welcome;
        put(welcome, static_cast<uint8_t>(PacketType::welcome));
        put(welcome, peer);
        transport_->send(peer, welcome, Channel::reliable);

        // Replay every already-spawned networked object so the late-joiner catches
        // up on objects created before it connected (e.g. the host's own player).
        for (const auto& [net_id, info] : spawn_info_) {
            auto packet = build_spawn_packet(net_id);
            if (!packet.empty()) transport_->send(peer, packet, Channel::reliable);
        }
    }
    // on_peer_connected (spawning this peer's player) runs AFTER the replay, so the
    // new peer's own spawn is broadcast on top of the existing-object catch-up.
    if (on_peer_connected) on_peer_connected(peer);
}

void NetManager::handle_disconnect(PeerId peer) {
    if (on_peer_disconnected) on_peer_disconnected(peer);
}

void NetManager::handle_receive(Scene& scene, ScriptManager& scripts,
                                ResourceManager& resources,
                                PeerId peer, std::span<const uint8_t> data,
                                Channel /*channel*/) {
    uint8_t type = 0;
    if (!take(data, type)) return;

    switch (static_cast<PacketType>(type)) {
    case PacketType::state_update:
        apply_state_update(scene, scripts, peer, data);
        break;
    case PacketType::spawn:
        apply_spawn(scene, resources, data);
        break;
    case PacketType::despawn:
        apply_despawn(scene, data);
        break;
    case PacketType::rpc_call:
        apply_rpc(scene, scripts, data);
        break;
    case PacketType::welcome:
        apply_welcome(data);
        break;
    default:
        FJELL_CORE_WARN("NetManager: unknown packet type {}", type);
        break;
    }
}

void NetManager::apply_state_update(Scene& scene, ScriptManager& scripts,
                                    PeerId sender, std::span<const uint8_t> data) {
    uint32_t tick = 0;
    uint16_t object_count = 0;
    if (!take(data, tick)) return;
    if (!take(data, object_count)) return;

    // The server relays client-authored updates to the other clients. Rebuild a
    // forward packet as we go (only the objects we accept from this sender).
    const bool relay = (role_ == NetRole::server);
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
        if (role_ == NetRole::server) {
            accept = (owner_of(net_id) == sender) && (sender != SERVER_PEER);
            owned_local = false;  // server snaps client-owned objects directly
        } else {
            owned_local = is_owned_by_local(net_id);
            accept = !owned_local;
        }

        uint32_t node_id = node_id_for(net_id);
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
                Node3D* spatial = node->as_spatial();
                if (glm::distance(spatial->transform.position, position) > snap_threshold_) {
                    spatial->transform.position = position;
                    spatial->transform.rotation = rotation;
                    corrections_[net_id].active = false;
                } else {
                    corrections_[net_id] = {position, rotation, true};
                }
            } else {
                // Remote object we don't own — smooth between 30 Hz snapshots
                // instead of snapping. Runs on the client (host's player) AND on
                // the server (each client's player), so motion looks identical in
                // every window. interpolate_remotes drives the actual transform.
                push_interp_sample(net_id, position, rotation);
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
    if (relay && fwd_count > 0 && transport_) {
        std::memcpy(fwd.data() + fwd_count_pos, &fwd_count, sizeof(fwd_count));
        transport_->broadcast(fwd, Channel::unreliable);
    }
}

void NetManager::push_interp_sample(NetId net_id, const glm::vec3& pos,
                                    const glm::quat& rot) {
    InterpBuffer& buf = interp_[net_id];

    // Stamp on a per-object monotonic timeline: normally exactly one nominal tick
    // past this object's previous sample, independent of arrival jitter.
    //
    // But re-anchor ahead of the render clock whenever the object's own newest
    // sample has fallen behind it. That happens (a) for a brand-new object that
    // joins an already-running session, and (b) for an existing object that went
    // idle — no transform change means no samples, so the render clock marches
    // past its stale ring while net_time_primed_ stays true and buf.count stays
    // > 0. In both cases, continuing from "newest + tick" would stamp the new
    // sample in the past, leaving the object frozen on a stale sample until the
    // clock crawled back up to it — the "cranky for a few seconds, then smooth"
    // feel. Anchoring to net_time_ + interp_delay_ puts the fresh sample exactly
    // one interp delay ahead of render_time, the steady-state relationship.
    float t;
    float prev_newest = -1.0f;
    if (buf.count > 0) {
        const size_t newest = (buf.head + INTERP_HISTORY - 1) % INTERP_HISTORY;
        prev_newest = buf.samples[newest].t;
    }

    if (!net_time_primed_) {
        // Very first sample of the session: anchor the shared timeline here.
        t = std::max(newest_sample_t_, tick_interval_);
    } else if (prev_newest >= net_time_) {
        // Continuous stream: keep the monotonic per-object cadence.
        t = prev_newest + tick_interval_;
    } else {
        // New or resuming-after-idle: re-anchor just ahead of the render clock.
        t = net_time_ + interp_delay_;
    }

    buf.samples[buf.head] = {t, pos, rot};
    buf.head = (buf.head + 1) % INTERP_HISTORY;
    if (buf.count < INTERP_HISTORY) ++buf.count;

    newest_sample_t_ = std::max(newest_sample_t_, t);

    // Seed the render clock the first time we ever see a sample: place it
    // interp_delay_ behind, so it starts inside the (about-to-fill) buffer.
    if (!net_time_primed_) {
        net_time_ = t - interp_delay_;
        net_time_primed_ = true;
    }
}

void NetManager::interpolate_remotes(Scene& scene, float dt) {
    // Both peers interpolate the objects they don't own: the client smooths the
    // host's player, the server smooths each client's player. Offline does nothing.
    if (role_ == NetRole::none || !net_time_primed_) return;

    // Advance the render clock at a constant nominal rate (one second of timeline
    // per real second). It must NOT chase the newest stamp directly: that target
    // steps up a whole tick on each (jittery) arrival, so a proportional pull
    // springs back and forth → rubberbanding. Instead run free and apply only a
    // gentle correction when buffer depth drifts outside a dead-band around
    // interp_delay_. Inside the band, motion is perfectly linear.
    net_time_ += dt;
    const float depth = newest_sample_t_ - net_time_;  // how far behind newest we render
    const float lo = interp_delay_ - tick_interval_;   // dead-band: ±1 tick of slack
    const float hi = interp_delay_ + tick_interval_;
    if (depth < lo) {
        net_time_ -= (lo - depth) * std::min(1.0f, 2.0f * dt);  // about to starve, ease back
    } else if (depth > hi) {
        net_time_ += (depth - hi) * std::min(1.0f, 2.0f * dt);  // buffer grew, ease forward
    }
    const float render_time = net_time_;

    // Reconcile predicted (owned) objects toward the last authoritative state.
    // Easing the predicted base toward the server hides small divergences; the
    // script's own movement this frame still applies on top, so input stays
    // responsive. Large errors were already hard-snapped in apply_state_update.
    const float blend = std::min(1.0f, correction_rate_ * dt);
    for (auto& [net_id, corr] : corrections_) {
        if (!corr.active) continue;
        uint32_t node_id = node_id_for(net_id);
        Node* node = (node_id != 0) ? scene.find(node_id) : nullptr;
        Node3D* spatial = node ? node->as_spatial() : nullptr;
        if (!spatial) continue;
        spatial->transform.position =
            glm::mix(spatial->transform.position, corr.position, blend);
        spatial->transform.rotation =
            glm::slerp(spatial->transform.rotation, corr.rotation, blend);
    }

    for (auto& [net_id, buf] : interp_) {
        if (is_owned_by_local(net_id)) continue;  // owned: driven by input/prediction
        if (buf.count == 0) continue;

        uint32_t node_id = node_id_for(net_id);
        Node* node = (node_id != 0) ? scene.find(node_id) : nullptr;
        Node3D* spatial = node ? node->as_spatial() : nullptr;
        if (!spatial) continue;

        // Find the pair of samples that brackets render_time. Samples are ordered
        // oldest→newest as (head, head+1, ...) modulo the ring.
        const size_t oldest = buf.head + (INTERP_HISTORY - buf.count);
        const InterpSnapshot& first = buf.samples[oldest % INTERP_HISTORY];
        const InterpSnapshot& last  =
            buf.samples[(buf.head + INTERP_HISTORY - 1) % INTERP_HISTORY];

        glm::vec3 pos = last.position;
        glm::quat rot = last.rotation;
        if (render_time <= first.t) {
            pos = first.position;  // before history → hold oldest (buffer filling)
            rot = first.rotation;
        } else if (render_time < last.t) {
            for (size_t k = 1; k < buf.count; ++k) {
                const InterpSnapshot& a = buf.samples[(oldest + k - 1) % INTERP_HISTORY];
                const InterpSnapshot& b = buf.samples[(oldest + k) % INTERP_HISTORY];
                if (render_time <= b.t && b.t > a.t) {
                    const float alpha = std::clamp((render_time - a.t) / (b.t - a.t),
                                                   0.0f, 1.0f);
                    pos = glm::mix(a.position, b.position, alpha);
                    rot = glm::slerp(a.rotation, b.rotation, alpha);
                    break;
                }
            }
        }
        // else render_time >= last.t → hold newest (starved; no extrapolation).

        spatial->transform.position = pos;
        spatial->transform.rotation = rot;
    }
}

void NetManager::apply_welcome(std::span<const uint8_t> data) {
    PeerId assigned = SERVER_PEER;
    if (!take(data, assigned)) return;
    if (role_ != NetRole::client) return;  // only a client adopts a server-assigned id
    local_peer_ = assigned;
    FJELL_CORE_INFO("NetManager: assigned peer id {}", local_peer_);
}

void NetManager::apply_spawn(Scene& scene, ResourceManager& resources,
                             std::span<const uint8_t> data) {
    NetId net_id = NET_ID_NONE;
    PeerId owner = SERVER_PEER;
    std::string preset_path;
    glm::vec3 position{};
    if (!take(data, net_id)) return;
    if (!take(data, owner)) return;
    if (!take_string(data, preset_path)) return;
    if (!take(data, position)) return;

    // Already have it (duplicate/reordered reliable packet)? Ignore.
    if (node_id_for(net_id) != 0) return;

    auto handle = resources.load_preset_asset(preset_path, rx_project_);
    if (!handle) {
        FJELL_CORE_ERROR("NetManager: spawn of net {} failed, can't load '{}'",
                         net_id, preset_path);
        return;
    }
    PresetAsset* asset = resources.get(*handle);
    if (!asset) return;

    Transform offset;
    offset.position = position;
    uint32_t node_id = asset->preset.instantiate(scene, -1, offset);
    if (node_id == 0) return;

    register_net_object(net_id, node_id);
    set_owner(net_id, owner);
    FJELL_CORE_INFO("NetManager: spawned net {} from '{}' (owner {})", net_id, preset_path, owner);
}

void NetManager::apply_despawn(Scene& scene, std::span<const uint8_t> data) {
    NetId net_id = NET_ID_NONE;
    if (!take(data, net_id)) return;

    uint32_t node_id = node_id_for(net_id);
    if (node_id != 0) scene.queue_destroy(node_id);
    unregister_net_object(net_id);
    FJELL_CORE_INFO("NetManager: despawned net {}", net_id);
}

} // namespace fjell
