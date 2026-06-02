#include "net/net_manager.hpp"

#include "net/enet_transport.hpp"
#include "core/log.hpp"
#include "scene/scene.hpp"
#include "scene/node.hpp"
#include "scripting/script_manager.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace fjell {
namespace {

// Append raw bytes / trivially-copyable values to a byte buffer.
template <typename T>
void put(std::vector<uint8_t>& buf, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    buf.insert(buf.end(), p, p + sizeof(T));
}

void put_bytes(std::vector<uint8_t>& buf, const uint8_t* data, size_t n) {
    buf.insert(buf.end(), data, data + n);
}

// Read a trivially-copyable value from a span at an offset, advancing it.
// Returns false if the span is too short (truncated/malformed packet).
template <typename T>
[[nodiscard]] bool take(std::span<const uint8_t>& sp, T& out) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (sp.size() < sizeof(T)) return false;
    std::memcpy(&out, sp.data(), sizeof(T));
    sp = sp.subspan(sizeof(T));
    return true;
}

} // namespace

NetManager::NetManager() = default;
NetManager::~NetManager() { shutdown(); }

// ── Lifecycle ───────────────────────────────────────────────────────────

void NetManager::host(uint16_t port, uint32_t max_clients) {
    shutdown();

    auto transport = std::make_unique<EnetTransport>();
    transport->on_connect    = [this](PeerId p) { handle_connect(p); };
    transport->on_disconnect = [this](PeerId p) { handle_disconnect(p); };
    transport->on_receive    = [this](PeerId p, std::span<const uint8_t> d, Channel c) {
        if (rx_scene_ && rx_scripts_) handle_receive(*rx_scene_, *rx_scripts_, p, d, c);
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
        if (rx_scene_ && rx_scripts_) handle_receive(*rx_scene_, *rx_scripts_, p, d, c);
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
    next_net_id_ = 1;
    net_to_node_.clear();
    node_to_net_.clear();
    ownership_.clear();
    shadow_.clear();
    rx_scene_ = nullptr;
    rx_scripts_ = nullptr;
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

} // namespace

void NetManager::ensure_scene_registered(Scene& scene, ScriptManager& scripts) {
    if (!net_to_node_.empty()) return;

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

void NetManager::receive_and_apply(Scene& scene, ScriptManager& scripts) {
    if (!transport_) return;
    ensure_scene_registered(scene, scripts);
    rx_scene_ = &scene;
    rx_scripts_ = &scripts;
    transport_->poll(0.0f);
}

void NetManager::collect_and_send(Scene& scene, ScriptManager& scripts) {
    if (!transport_) return;
    ensure_scene_registered(scene, scripts);

    // Only the authority sends state. For v1 that's the server (it owns the
    // truth for every object); owned-client send arrives with prediction.
    if (role_ != NetRole::server) {
        transport_->flush();
        return;
    }

    ++server_tick_;

    std::vector<uint8_t> packet;
    put(packet, static_cast<uint8_t>(PacketType::state_update));
    put(packet, server_tick_);
    const size_t count_pos = packet.size();
    put(packet, static_cast<uint16_t>(0));  // object_count placeholder

    uint16_t object_count = 0;

    for (const auto& [net_id, node_id] : net_to_node_) {
        Node* node = scene.find(node_id);
        if (!node) continue;

        const uint8_t* instance = nullptr;
        auto descs = replicated_descs(*node, scripts, instance);

        // Build this object's current replicated bytes (fields then transform).
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
        bool transform_changed = has_transform;
        if (has_transform && !first_send) {
            transform_changed =
                std::memcmp(current.data() + off, shadow.data() + off,
                            current.size() - off) != 0;
        }

        if (dirty_mask == 0 && !transform_changed) continue;  // unchanged, skip

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

        shadow = std::move(current);
        ++object_count;
    }

    if (object_count > 0) {
        std::memcpy(packet.data() + count_pos, &object_count, sizeof(object_count));
        transport_->broadcast(packet, Channel::unreliable);
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
}

uint32_t NetManager::node_id_for(NetId net_id) const {
    auto it = net_to_node_.find(net_id);
    return it != net_to_node_.end() ? it->second : 0;
}

NetId NetManager::net_id_for(uint32_t node_id) const {
    auto it = node_to_net_.find(node_id);
    return it != node_to_net_.end() ? it->second : NET_ID_NONE;
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

// ── Transport callbacks ─────────────────────────────────────────────────────

void NetManager::handle_connect(PeerId peer) {
    if (on_peer_connected) on_peer_connected(peer);
}

void NetManager::handle_disconnect(PeerId peer) {
    if (on_peer_disconnected) on_peer_disconnected(peer);
}

void NetManager::handle_receive(Scene& scene, ScriptManager& scripts,
                                PeerId /*peer*/, std::span<const uint8_t> data,
                                Channel /*channel*/) {
    uint8_t type = 0;
    if (!take(data, type)) return;

    switch (static_cast<PacketType>(type)) {
    case PacketType::state_update:
        apply_state_update(scene, scripts, data);
        break;
    default:
        FJELL_CORE_WARN("NetManager: unknown packet type {}", type);
        break;
    }
}

void NetManager::apply_state_update(Scene& scene, ScriptManager& scripts,
                                    std::span<const uint8_t> data) {
    uint32_t tick = 0;
    uint16_t object_count = 0;
    if (!take(data, tick)) return;
    if (!take(data, object_count)) return;

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

        // Resolve the local node. A client must not apply state to objects it
        // has authority over (that would clobber its own prediction).
        const bool apply = !is_local_authority(net_id);
        uint32_t node_id = node_id_for(net_id);
        Node* node = (node_id != 0) ? scene.find(node_id) : nullptr;

        const uint8_t* cinst = nullptr;
        auto descs = node ? replicated_descs(*node, scripts, cinst)
                          : std::vector<const OwnedFieldDesc*>{};

        // Walk every set bit, consuming its bytes from the packet regardless of
        // whether we apply them — the stream must stay aligned.
        for (size_t i = 0; i < descs.size(); ++i) {
            if (!(dirty_mask & (1u << i))) continue;
            const uint32_t sz = descs[i]->size;
            if (data.size() < sz) return;
            if (apply && cinst) {
                auto* dst = const_cast<uint8_t*>(cinst) + descs[i]->offset;
                std::memcpy(dst, data.data(), sz);
            }
            data = data.subspan(sz);
        }

        if (apply && has_transform) {
            if (Node3D* spatial = node ? node->as_spatial() : nullptr) {
                spatial->transform.position = position;
                spatial->transform.rotation = rotation;
            }
        }
    }
}

} // namespace fjell
