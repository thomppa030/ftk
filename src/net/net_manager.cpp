#include "net/net_manager.hpp"

#include "net/enet_transport.hpp"
#include "core/log.hpp"

namespace fjell {

NetManager::NetManager() = default;
NetManager::~NetManager() { shutdown(); }

// ── Lifecycle ───────────────────────────────────────────────────────────

void NetManager::host(uint16_t port, uint32_t max_clients) {
    shutdown();

    auto transport = std::make_unique<EnetTransport>();
    transport->on_connect    = [this](PeerId p) { handle_connect(p); };
    transport->on_disconnect = [this](PeerId p) { handle_disconnect(p); };
    transport->on_receive    = [this](PeerId p, std::span<const uint8_t> d, Channel c) {
        handle_receive(p, d, c);
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
        handle_receive(p, d, c);
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
    next_net_id_ = 1;
    net_to_node_.clear();
    node_to_net_.clear();
    ownership_.clear();
}

// ── Per-frame pumps ───────────────────────────────────────────────────────

void NetManager::receive_and_apply(Scene& /*scene*/) {
    if (!transport_) return;
    transport_->poll(0.0f);
    // State application lands in PR 2 (replication). For now polling drives the
    // connect/disconnect/receive callbacks.
}

void NetManager::collect_and_send(Scene& /*scene*/) {
    if (!transport_) return;
    // Dirty-field collection lands in PR 2. For now just flush queued packets.
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
    // The server is authoritative over everything it isn't explicitly handing
    // to a client; a client has authority only over objects it owns.
    if (role_ == NetRole::server) return true;
    if (role_ == NetRole::client) return owner_of(net_id) == local_peer_;
    return true;  // single-player: always authoritative
}

// ── Transport callbacks ─────────────────────────────────────────────────────

void NetManager::handle_connect(PeerId peer) {
    if (on_peer_connected) on_peer_connected(peer);
}

void NetManager::handle_disconnect(PeerId peer) {
    if (on_peer_disconnected) on_peer_disconnected(peer);
}

void NetManager::handle_receive(PeerId /*peer*/, std::span<const uint8_t> /*data*/,
                                Channel /*channel*/) {
    // Packet decoding (STATE_UPDATE / RPC_CALL / SPAWN / DESPAWN) lands in later
    // PRs. PR 1 establishes the connection; nothing is sent yet.
}

} // namespace fjell
