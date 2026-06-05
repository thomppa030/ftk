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
        if (rx_ctx_) handle_receive(*rx_ctx_, p, d, c);
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
        if (rx_ctx_) handle_receive(*rx_ctx_, p, d, c);
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
    net_scene_.clear();
    replication_.clear();
    prediction_.clear();
    spawn_replicator_.clear();
    rx_ctx_ = nullptr;
}

// ── Per-frame pumps ───────────────────────────────────────────────────────

void NetManager::receive_and_apply(Scene& scene, ScriptManager& scripts,
                                   ResourceManager& resources, const Project* project) {
    if (!transport_) return;
    replication_.ensure_scene_registered(scene, scripts, net_scene_);

    // The receive callback (fired inside poll()) reads its collaborators from
    // rx_ctx_; this NetContext is valid only for the poll below, then cleared.
    NetContext ctx{&scene, &scripts, &resources, project};
    rx_ctx_ = &ctx;

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

    rx_ctx_ = nullptr;
}

void NetManager::collect_and_send(Scene& scene, ScriptManager& scripts) {
    replication_.collect_and_send(scene, scripts, net_scene_, transport_.get(),
                                  role_, local_peer_);
}

// ── Network identity ──────────────────────────────────────────────────────

NetId NetManager::assign_net_id() {
    return net_scene_.assign_net_id();
}

void NetManager::register_net_object(NetId net_id, uint32_t node_id) {
    net_scene_.register_object(net_id, node_id);
}

void NetManager::unregister_net_object(NetId net_id) {
    // Identity + ownership live in NetScene; the other per-object state still
    // lives on the facade until those components are extracted, so clear it here.
    net_scene_.unregister_object(net_id);
    replication_.erase(net_id);
    prediction_.erase(net_id);
    spawn_replicator_.erase(net_id);
}

// ── Spawn / despawn ─────────────────────────────────────────────────────────

NetId NetManager::spawn(Scene& scene, ResourceManager& resources,
                        const std::string& preset_path, const glm::vec3& position,
                        PeerId owner, const Project* project) {
    if (role_ != NetRole::server) {
        FJELL_CORE_WARN("NetManager: spawn() called off the server, ignoring");
        return NET_ID_NONE;
    }
    return spawn_replicator_.spawn(scene, resources, net_scene_, transport_.get(),
                                   preset_path, position, owner, project);
}

void NetManager::destroy(Scene& scene, NetId net_id) {
    if (role_ != NetRole::server) {
        FJELL_CORE_WARN("NetManager: destroy() called off the server, ignoring");
        return;
    }
    spawn_replicator_.destroy(scene, net_scene_, transport_.get(), net_id);
    unregister_net_object(net_id);  // cross-component cleanup (identity/prediction/shadow)
}

uint32_t NetManager::node_id_for(NetId net_id) const {
    return net_scene_.node_id_for(net_id);
}

NetId NetManager::net_id_for(uint32_t node_id) const {
    return net_scene_.net_id_for(node_id);
}

// ── RPC ─────────────────────────────────────────────────────────────────────

void NetManager::send_rpc(Scene& scene, ScriptManager& scripts, uint32_t node_id,
                          uint16_t rpc_index, const uint8_t* payload, uint32_t size) {
    rpc_router_.send(scene, scripts, net_scene_, transport_.get(), role_,
                     node_id, rpc_index, payload, size);
}

// ── Ownership ─────────────────────────────────────────────────────────────

void NetManager::set_owner(NetId net_id, PeerId peer) {
    net_scene_.set_owner(net_id, peer);
}

PeerId NetManager::owner_of(NetId net_id) const {
    return net_scene_.owner_of(net_id);
}

bool NetManager::is_local_authority(NetId net_id) const {
    return net_scene_.is_local_authority(net_id, role_, local_peer_);
}

bool NetManager::is_owned_by_local(NetId net_id) const {
    return net_scene_.is_owned_by_local(net_id, role_, local_peer_);
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
        spawn_replicator_.replay_to(*transport_, peer);
    }
    // on_peer_connected (spawning this peer's player) runs AFTER the replay, so the
    // new peer's own spawn is broadcast on top of the existing-object catch-up.
    if (on_peer_connected) on_peer_connected(peer);
}

void NetManager::handle_disconnect(PeerId peer) {
    if (on_peer_disconnected) on_peer_disconnected(peer);
}

void NetManager::handle_receive(const NetContext& ctx, PeerId peer,
                                std::span<const uint8_t> data, Channel /*channel*/) {
    uint8_t type = 0;
    if (!take(data, type)) return;

    switch (static_cast<PacketType>(type)) {
    case PacketType::state_update:
        replication_.apply_state_update(*ctx.scene, *ctx.scripts, net_scene_, prediction_,
                                        transport_.get(), role_, peer, local_peer_, data);
        break;
    case PacketType::spawn:
        spawn_replicator_.apply_spawn(*ctx.scene, *ctx.resources, net_scene_, ctx.project, data);
        break;
    case PacketType::despawn: {
        NetId despawned = spawn_replicator_.apply_despawn(*ctx.scene, net_scene_, data);
        if (despawned != NET_ID_NONE) unregister_net_object(despawned);
        break;
    }
    case PacketType::rpc_call:
        rpc_router_.apply(*ctx.scene, *ctx.scripts, net_scene_, transport_.get(), role_, data);
        break;
    case PacketType::welcome:
        apply_welcome(data);
        break;
    default:
        FJELL_CORE_WARN("NetManager: unknown packet type {}", type);
        break;
    }
}

void NetManager::interpolate_remotes(Scene& scene, float dt) {
    prediction_.apply(scene, net_scene_, role_, local_peer_, dt);
}

void NetManager::apply_welcome(std::span<const uint8_t> data) {
    PeerId assigned = SERVER_PEER;
    if (!take(data, assigned)) return;
    if (role_ != NetRole::client) return;  // only a client adopts a server-assigned id
    local_peer_ = assigned;
    FJELL_CORE_INFO("NetManager: assigned peer id {}", local_peer_);
}

} // namespace fjell
