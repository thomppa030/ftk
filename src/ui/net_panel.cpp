#include "ui/net_panel.hpp"

#include "net/net_manager.hpp"

#include <imgui.h>

namespace fjell {
namespace {

const char* role_label(NetRole role) {
    switch (role) {
    case NetRole::server: return "Host";
    case NetRole::client: return "Client";
    default:              return "Offline";
    }
}

} // namespace

void NetPanel::draw(float dt, const char* title) {
    if (!ImGui::Begin(title)) {
        ImGui::End();
        return;
    }

    if (!net_) {
        ImGui::TextDisabled("No network manager.");
        ImGui::End();
        return;
    }

    const NetRole role = net_->role();
    ImGui::Text("Role: %s", role_label(role));
    if (net_->is_connected()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(peer %u)", net_->local_peer());
    }

    // ── Per-second rates (diff the cumulative counters once a second) ────
    sample_timer_ += dt;
    if (sample_timer_ >= 1.0f) {
        const TransportStats cur = net_->stats();
        const float inv = 1.0f / sample_timer_;
        send_bps_ = static_cast<float>(cur.bytes_sent   - last_sample_.bytes_sent)   * inv;
        recv_bps_ = static_cast<float>(cur.bytes_recv   - last_sample_.bytes_recv)   * inv;
        send_pps_ = static_cast<float>(cur.packets_sent - last_sample_.packets_sent) * inv;
        recv_pps_ = static_cast<float>(cur.packets_recv - last_sample_.packets_recv) * inv;
        last_sample_ = cur;
        sample_timer_ = 0.0f;

        send_history_[history_index_] = send_bps_;
        history_index_ = (history_index_ + 1) % BANDWIDTH_HISTORY;
    }

    ImGui::SeparatorText("Bandwidth");
    ImGui::Text("Out: %.1f KB/s  (%.0f pkt/s)", send_bps_ / 1024.0f, send_pps_);
    ImGui::Text("In:  %.1f KB/s  (%.0f pkt/s)", recv_bps_ / 1024.0f, recv_pps_);
    ImGui::PlotLines("##bw", send_history_.data(), BANDWIDTH_HISTORY, history_index_,
                     "out B/s", 0.0f, FLT_MAX, ImVec2(0, 40));

    // ── Connected peers + link quality ──────────────────────────────────
    ImGui::SeparatorText("Peers");
    const auto peers = net_->peers();
    if (peers.empty()) {
        ImGui::TextDisabled("No connected peers.");
    } else if (ImGui::BeginTable("##peers", 3,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Peer");
        ImGui::TableSetupColumn("RTT");
        ImGui::TableSetupColumn("Loss");
        ImGui::TableHeadersRow();
        for (const auto& p : peers) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%u", p.id);
            ImGui::TableNextColumn(); ImGui::Text("%u ms", p.rtt_ms);
            ImGui::TableNextColumn(); ImGui::Text("%.1f%%", p.loss_pct);
        }
        ImGui::EndTable();
    }

    // ── Network conditioning (testing) ──────────────────────────────────
    ImGui::SeparatorText("Conditioning (simulated)");
    bool changed = false;
    changed |= ImGui::SliderFloat("Latency", &sim_latency_ms_, 0.0f, 300.0f, "%.0f ms");
    changed |= ImGui::SliderFloat("Jitter",  &sim_jitter_ms_,  0.0f, 100.0f, "%.0f ms");
    changed |= ImGui::SliderFloat("Loss",    &sim_loss_pct_,   0.0f, 30.0f,  "%.1f %%");
    if (changed) {
        net_->set_conditioning(sim_latency_ms_, sim_jitter_ms_, sim_loss_pct_);
    }
    if (!net_->is_connected()) {
        ImGui::TextDisabled("Connect first; conditioning applies to the live session.");
    }

    ImGui::End();
}

} // namespace fjell
