// transport.hpp — Abstract networking transport for the multiplayer layer.
//
// The transport moves opaque byte buffers between peers over two channels
// (reliable + unreliable). Everything above it (NetManager, replication, RPCs)
// is transport-agnostic; ENet is the only implementation for now, but the
// interface lets us swap it (e.g. for Steam sockets) without touching gameplay.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace fjell {

// Stable identifier for a connected peer. The host is always SERVER_PEER.
using PeerId = uint32_t;
constexpr PeerId SERVER_PEER = 0;

enum class Channel : uint8_t {
    reliable = 0,    // RPCs, spawn/despawn, important state — guaranteed, ordered
    unreliable = 1,  // replicated field updates — latest-wins, may drop
};

constexpr uint8_t CHANNEL_COUNT = 2;

// Cumulative byte/packet counters since the session began. The net panel samples
// these once a second and diffs to show per-second rates.
struct TransportStats {
    uint64_t bytes_sent{0};
    uint64_t bytes_recv{0};
    uint64_t packets_sent{0};
    uint64_t packets_recv{0};
};

// Per-connected-peer link quality, surfaced for the net panel.
struct PeerInfo {
    PeerId   id{SERVER_PEER};
    uint32_t rtt_ms{0};    // mean round-trip time
    float    loss_pct{0};  // mean packet loss, 0..100
};

class Transport {
public:
    virtual ~Transport() = default;

    // Start listening as the host. Returns false on failure (port in use, etc.).
    [[nodiscard]] virtual bool host(uint16_t port, uint32_t max_clients) = 0;

    // Connect to a host as a client. Returns false if the connect could not be
    // initiated; actual connection success arrives later via on_connect.
    [[nodiscard]] virtual bool connect(const std::string& address, uint16_t port) = 0;

    // Tear down host/client state and drop all peers.
    virtual void disconnect() = 0;

    // Pump the transport: drain incoming packets and fire callbacks. Called
    // every frame. timeout_ms blocks up to that long waiting for an event.
    virtual void poll(float timeout_ms = 0.0f) = 0;

    virtual void send(PeerId peer, std::span<const uint8_t> data, Channel channel) = 0;
    virtual void broadcast(std::span<const uint8_t> data, Channel channel) = 0;

    // Force-flush queued outgoing packets immediately (otherwise sent on poll).
    virtual void flush() = 0;

    // ── Observability (for the net panel) ───────────────────────────────
    // Default to empty so non-instrumented transports need not implement them.
    [[nodiscard]] virtual TransportStats stats() const { return {}; }
    [[nodiscard]] virtual std::vector<PeerInfo> peers() const { return {}; }

    // ── Network conditioning (testing tool, no-op unless overridden) ─────
    // Inject artificial latency/jitter/loss locally so v1 interpolation and
    // prediction can be validated under realistic conditions from one machine.
    virtual void set_conditioning(float /*latency_ms*/, float /*jitter_ms*/,
                                  float /*loss_pct*/) {}

    // Callbacks, set by the owner (NetManager) before host()/connect().
    std::function<void(PeerId)> on_connect;
    std::function<void(PeerId)> on_disconnect;
    std::function<void(PeerId, std::span<const uint8_t>, Channel)> on_receive;

protected:
    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;
};

} // namespace fjell
