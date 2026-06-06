// net_panel.hpp — Debug/testing panel for the multiplayer session.
//
// Read-only observability (role, peers, RTT/loss, bytes & packets per second)
// plus the network-conditioning sliders (latency / jitter / loss) that inject
// artificial link degradation so interpolation and prediction can be validated
// from a single machine. Launch (host/join) lives on the viewport Net dropdown,
// not here.
#pragma once

#include "net/transport.hpp"

#include <array>

namespace fjell {

class NetManager;

class NetPanel {
public:
    void init(NetManager* net) { net_ = net; }
    void draw(float dt, const char* title);

private:
    NetManager* net_{nullptr};

    // Rolling per-second rates, sampled by diffing the cumulative counters once
    // a second.
    TransportStats last_sample_{};
    float sample_timer_{0.0f};
    float send_bps_{0.0f};
    float recv_bps_{0.0f};
    float send_pps_{0.0f};
    float recv_pps_{0.0f};

    // Conditioning slider state (pushed to the transport on change).
    float sim_latency_ms_{0.0f};
    float sim_jitter_ms_{0.0f};
    float sim_loss_pct_{0.0f};

    static constexpr int BANDWIDTH_HISTORY = 120;
    std::array<float, BANDWIDTH_HISTORY> send_history_{};
    int history_index_{0};
};

} // namespace fjell
