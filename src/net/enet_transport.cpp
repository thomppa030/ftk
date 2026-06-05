#include "net/enet_transport.hpp"

#include "core/log.hpp"

#include <enet/enet.h>

#include <cstring>

namespace fjell {

namespace {
// Refcount for the global ENet init — lets host + client coexist in one process.
int g_enet_refcount = 0;

ENetPeer* as_peer(void* p) { return static_cast<ENetPeer*>(p); }
ENetHost* as_host(void* p) { return static_cast<ENetHost*>(p); }

// Encode a PeerId into ENetPeer::data without allocating (fits in the pointer).
void set_peer_id(ENetPeer* peer, PeerId id) {
    peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
}
PeerId get_peer_id(ENetPeer* peer) {
    return static_cast<PeerId>(reinterpret_cast<uintptr_t>(peer->data));
}
} // namespace

// ── EnetGlobal ──────────────────────────────────────────────────────────

EnetGlobal::EnetGlobal() {
    if (g_enet_refcount++ == 0) {
        if (enet_initialize() != 0) {
            FJELL_CORE_ERROR("ENet: enet_initialize() failed");
            g_enet_refcount = 0;
        }
    }
}

EnetGlobal::~EnetGlobal() {
    if (--g_enet_refcount == 0) {
        enet_deinitialize();
    }
    if (g_enet_refcount < 0) {
        g_enet_refcount = 0;
    }
}

// ── EnetTransport ───────────────────────────────────────────────────────

EnetTransport::~EnetTransport() {
    disconnect();
}

bool EnetTransport::host(uint16_t port, uint32_t max_clients) {
    if (host_) {
        FJELL_CORE_WARN("ENet: host() called while already active; disconnecting first");
        disconnect();
    }

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;

    host_ = enet_host_create(&address, max_clients, CHANNEL_COUNT, 0, 0);
    if (!host_) {
        FJELL_CORE_ERROR("ENet: failed to create host on port {}", port);
        return false;
    }
    is_host_ = true;
    FJELL_CORE_INFO("ENet: hosting on port {} (max {} clients)", port, max_clients);
    return true;
}

bool EnetTransport::connect(const std::string& address, uint16_t port) {
    if (host_) {
        FJELL_CORE_WARN("ENet: connect() called while already active; disconnecting first");
        disconnect();
    }

    host_ = enet_host_create(nullptr, 1, CHANNEL_COUNT, 0, 0);
    if (!host_) {
        FJELL_CORE_ERROR("ENet: failed to create client host");
        return false;
    }

    ENetAddress addr{};
    if (enet_address_set_host(&addr, address.c_str()) != 0) {
        FJELL_CORE_ERROR("ENet: could not resolve address '{}'", address);
        enet_host_destroy(as_host(host_));
        host_ = nullptr;
        return false;
    }
    addr.port = port;

    ENetPeer* server = enet_host_connect(as_host(host_), &addr, CHANNEL_COUNT, 0);
    if (!server) {
        FJELL_CORE_ERROR("ENet: no available peers to connect to {}:{}", address, port);
        enet_host_destroy(as_host(host_));
        host_ = nullptr;
        return false;
    }
    // The host is always SERVER_PEER from a client's point of view.
    set_peer_id(server, SERVER_PEER);
    is_host_ = false;
    FJELL_CORE_INFO("ENet: connecting to {}:{}", address, port);
    return true;
}

void EnetTransport::disconnect() {
    if (!host_) return;

    // Politely disconnect all peers, then give ENet a brief window to flush.
    for (auto& [id, peer] : peers_) {
        enet_peer_disconnect_now(as_peer(peer), 0);
    }
    peers_.clear();

    enet_host_destroy(as_host(host_));
    host_ = nullptr;
    is_host_ = false;
    next_peer_id_ = 1;
}

void EnetTransport::poll(float timeout_ms) {
    if (!host_) return;

    ENetEvent event{};
    int serviced = enet_host_service(as_host(host_),
                                     &event,
                                     static_cast<enet_uint32>(timeout_ms));
    while (serviced > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            PeerId id = is_host_ ? next_peer_id_++ : SERVER_PEER;
            set_peer_id(event.peer, id);
            peers_[id] = event.peer;
            FJELL_CORE_INFO("ENet: peer {} connected", id);
            if (on_connect) on_connect(id);
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            PeerId id = get_peer_id(event.peer);
            auto channel = static_cast<Channel>(event.channelID);
            stats_.bytes_recv += event.packet->dataLength;
            ++stats_.packets_recv;
            if (on_receive) {
                on_receive(id,
                           std::span<const uint8_t>(event.packet->data,
                                                    event.packet->dataLength),
                           channel);
            }
            enet_packet_destroy(event.packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            PeerId id = get_peer_id(event.peer);
            FJELL_CORE_INFO("ENet: peer {} disconnected", id);
            peers_.erase(id);
            event.peer->data = nullptr;
            if (on_disconnect) on_disconnect(id);
            break;
        }
        case ENET_EVENT_TYPE_NONE:
            break;
        }

        // Subsequent events are already queued; drain without blocking.
        serviced = enet_host_service(as_host(host_), &event, 0);
    }

    if (serviced < 0) {
        FJELL_CORE_ERROR("ENet: enet_host_service() failed");
    }
}

void EnetTransport::send(PeerId peer, std::span<const uint8_t> data, Channel channel) {
    ENetPeer* target = as_peer(peer_for(peer));
    if (!target) return;

    enet_uint32 flags = (channel == Channel::reliable) ? ENET_PACKET_FLAG_RELIABLE
                                                        : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(data.data(), data.size_bytes(), flags);
    if (!packet) return;
    if (enet_peer_send(target, static_cast<enet_uint8>(channel), packet) != 0) {
        // enet_peer_send takes ownership only on success; destroy on failure.
        enet_packet_destroy(packet);
        return;
    }
    stats_.bytes_sent += data.size_bytes();
    ++stats_.packets_sent;
}

void EnetTransport::broadcast(std::span<const uint8_t> data, Channel channel) {
    if (!host_) return;

    enet_uint32 flags = (channel == Channel::reliable) ? ENET_PACKET_FLAG_RELIABLE
                                                        : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(data.data(), data.size_bytes(), flags);
    if (!packet) return;
    enet_host_broadcast(as_host(host_), static_cast<enet_uint8>(channel), packet);

    // Count the wire bytes once per recipient so the rate reflects actual load.
    stats_.bytes_sent += data.size_bytes() * peers_.size();
    stats_.packets_sent += peers_.size();
}

void EnetTransport::flush() {
    if (host_) enet_host_flush(as_host(host_));
}

void* EnetTransport::peer_for(PeerId id) const {
    auto it = peers_.find(id);
    return it != peers_.end() ? it->second : nullptr;
}

std::vector<PeerInfo> EnetTransport::peers() const {
    std::vector<PeerInfo> out;
    out.reserve(peers_.size());
    for (const auto& [id, raw] : peers_) {
        const ENetPeer* p = as_peer(raw);
        // packetLoss is a ratio of ENET_PEER_PACKET_LOSS_SCALE; scale to percent.
        const float loss = 100.0f * static_cast<float>(p->packetLoss) /
                           static_cast<float>(ENET_PEER_PACKET_LOSS_SCALE);
        out.push_back({id, p->roundTripTime, loss});
    }
    return out;
}

} // namespace fjell
