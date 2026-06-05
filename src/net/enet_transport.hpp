// enet_transport.hpp — ENet-backed Transport implementation.
#pragma once

#include "net/transport.hpp"

#include <chrono>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

// ENet's own headers typedef these from anonymous structs, so we can't portably
// forward-declare them. The .cpp includes <enet/enet.h>; here we keep the
// pointers opaque via void* and cast in the implementation.

namespace fjell {

// RAII guard around enet_initialize()/enet_deinitialize(). Refcounted so
// multiple transports (e.g. host + client in one process for loopback tests)
// share a single global init.
class EnetGlobal {
public:
    EnetGlobal();
    ~EnetGlobal();
    EnetGlobal(const EnetGlobal&) = delete;
    EnetGlobal& operator=(const EnetGlobal&) = delete;
    EnetGlobal(EnetGlobal&&) = delete;
    EnetGlobal& operator=(EnetGlobal&&) = delete;
};

class EnetTransport final : public Transport {
public:
    EnetTransport() = default;
    ~EnetTransport() override;
    EnetTransport(const EnetTransport&) = delete;
    EnetTransport& operator=(const EnetTransport&) = delete;
    EnetTransport(EnetTransport&&) = delete;
    EnetTransport& operator=(EnetTransport&&) = delete;

    [[nodiscard]] bool host(uint16_t port, uint32_t max_clients) override;
    [[nodiscard]] bool connect(const std::string& address, uint16_t port) override;
    void disconnect() override;

    void poll(float timeout_ms = 0.0f) override;
    void send(PeerId peer, std::span<const uint8_t> data, Channel channel) override;
    void broadcast(std::span<const uint8_t> data, Channel channel) override;
    void flush() override;

    [[nodiscard]] TransportStats stats() const override { return stats_; }
    [[nodiscard]] std::vector<PeerInfo> peers() const override;

    void set_conditioning(float latency_ms, float jitter_ms, float loss_pct) override;

private:
    [[nodiscard]] void* peer_for(PeerId id) const;  // ENetPeer*

    // Roll for simulated loss (unreliable channel only).
    [[nodiscard]] bool should_drop(Channel channel);

    // Either dispatch immediately (no conditioning) or queue for delayed release.
    void enqueue_or_dispatch(PeerId peer, bool broadcast, Channel channel,
                             std::span<const uint8_t> data);

    // Hand a packet to ENet now. `broadcast` selects host-broadcast vs per-peer
    // send. Shared by the immediate path and the delayed-release path so
    // conditioning is transparent to the stats counters.
    void dispatch(PeerId peer, bool broadcast, Channel channel,
                  const std::vector<uint8_t>& bytes);

    // Release any conditioning-delayed packets whose time has come.
    void flush_delayed();

    EnetGlobal global_;
    void*      host_{nullptr};  // ENetHost*

    // Server: many peers, each assigned a PeerId. Client: one entry (the host).
    std::unordered_map<PeerId, void*> peers_;  // values are ENetPeer*
    PeerId next_peer_id_{1};   // SERVER_PEER (0) is reserved for the host
    bool   is_host_{false};

    TransportStats stats_;

    // Network conditioning (testing only). When latency/jitter are zero, packets
    // dispatch immediately; otherwise they queue and release after the delay.
    float sim_latency_ms_{0.0f};
    float sim_jitter_ms_{0.0f};
    float sim_loss_pct_{0.0f};

    using Clock = std::chrono::steady_clock;
    struct DelayedPacket {
        Clock::time_point release;
        PeerId            peer{SERVER_PEER};
        bool              broadcast{false};
        Channel           channel{Channel::unreliable};
        std::vector<uint8_t> bytes;
    };
    std::vector<DelayedPacket> delayed_;
    std::mt19937 rng_{0xC0FFEEu};  // fixed seed: reproducible loss patterns
};

} // namespace fjell
