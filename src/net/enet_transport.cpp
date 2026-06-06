#include "net/enet_transport.hpp"

#include "core/log.hpp"

#include <enet/enet.h>

#include <algorithm>
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
    delayed_.clear();  // drop conditioning-delayed packets bound for the dead host
}

void EnetTransport::poll(float timeout_ms) {
    if (!host_) return;

    flush_delayed();  // release any conditioning-delayed packets that are due

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

// Drop on the unreliable channel only, with probability sim_loss_pct_. Dropping
// a reliable packet would just make ENet resend it — a confusing slider — so
// loss applies to the state stream, which is the realistic case.
bool EnetTransport::should_drop(Channel channel) {
    if (sim_loss_pct_ <= 0.0f || channel == Channel::reliable) return false;
    std::uniform_real_distribution<float> d(0.0f, 100.0f);
    return d(rng_) < sim_loss_pct_;
}

void EnetTransport::send(PeerId peer, std::span<const uint8_t> data, Channel channel) {
    if (!peer_for(peer)) return;
    if (should_drop(channel)) return;
    enqueue_or_dispatch(peer, false, channel, data);
}

void EnetTransport::broadcast(std::span<const uint8_t> data, Channel channel) {
    if (!host_) return;
    if (should_drop(channel)) return;
    enqueue_or_dispatch(SERVER_PEER, true, channel, data);
}

void EnetTransport::enqueue_or_dispatch(PeerId peer, bool broadcast, Channel channel,
                                        std::span<const uint8_t> data) {
    // No conditioning → straight to the wire. Otherwise queue with a release time
    // latency ± jitter in the future; flush_delayed() releases it during poll().
    if (sim_latency_ms_ <= 0.0f && sim_jitter_ms_ <= 0.0f) {
        dispatch(peer, broadcast, channel, std::vector<uint8_t>(data.begin(), data.end()));
        return;
    }
    float delay = sim_latency_ms_;
    if (sim_jitter_ms_ > 0.0f) {
        std::uniform_real_distribution<float> j(-sim_jitter_ms_, sim_jitter_ms_);
        delay = std::max(0.0f, delay + j(rng_));
    }
    delayed_.push_back({Clock::now() + std::chrono::microseconds(
                            static_cast<int64_t>(delay * 1000.0f)),
                        peer, broadcast, channel,
                        std::vector<uint8_t>(data.begin(), data.end())});
}

void EnetTransport::dispatch(PeerId peer, bool broadcast, Channel channel,
                             const std::vector<uint8_t>& bytes) {
    enet_uint32 flags = (channel == Channel::reliable) ? ENET_PACKET_FLAG_RELIABLE
                                                        : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), flags);
    if (!packet) return;

    if (broadcast) {
        if (!host_) { enet_packet_destroy(packet); return; }
        enet_host_broadcast(as_host(host_), static_cast<enet_uint8>(channel), packet);
        stats_.bytes_sent += bytes.size() * peers_.size();
        stats_.packets_sent += peers_.size();
    } else {
        ENetPeer* target = as_peer(peer_for(peer));
        if (!target) { enet_packet_destroy(packet); return; }
        if (enet_peer_send(target, static_cast<enet_uint8>(channel), packet) != 0) {
            enet_packet_destroy(packet);  // ENet takes ownership only on success
            return;
        }
        stats_.bytes_sent += bytes.size();
        ++stats_.packets_sent;
    }
}

void EnetTransport::flush_delayed() {
    if (delayed_.empty()) return;
    const auto now = Clock::now();
    auto it = delayed_.begin();
    while (it != delayed_.end()) {
        if (it->release <= now) {
            dispatch(it->peer, it->broadcast, it->channel, it->bytes);
            it = delayed_.erase(it);
        } else {
            ++it;
        }
    }
}

void EnetTransport::set_conditioning(float latency_ms, float jitter_ms, float loss_pct) {
    sim_latency_ms_ = std::max(0.0f, latency_ms);
    sim_jitter_ms_  = std::max(0.0f, jitter_ms);
    sim_loss_pct_   = std::clamp(loss_pct, 0.0f, 100.0f);
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
