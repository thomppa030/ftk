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

namespace fjell {

// Stable identifier for a connected peer. The host is always SERVER_PEER.
using PeerId = uint32_t;
constexpr PeerId SERVER_PEER = 0;

enum class Channel : uint8_t {
    reliable = 0,    // RPCs, spawn/despawn, important state — guaranteed, ordered
    unreliable = 1,  // replicated field updates — latest-wins, may drop
};

constexpr uint8_t CHANNEL_COUNT = 2;

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
