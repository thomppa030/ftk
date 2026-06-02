// enet_transport.hpp — ENet-backed Transport implementation.
#pragma once

#include "net/transport.hpp"

#include <cstdint>
#include <unordered_map>

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

private:
    [[nodiscard]] void* peer_for(PeerId id) const;  // ENetPeer*

    EnetGlobal global_;
    void*      host_{nullptr};  // ENetHost*

    // Server: many peers, each assigned a PeerId. Client: one entry (the host).
    std::unordered_map<PeerId, void*> peers_;  // values are ENetPeer*
    PeerId next_peer_id_{1};   // SERVER_PEER (0) is reserved for the host
    bool   is_host_{false};
};

} // namespace fjell
