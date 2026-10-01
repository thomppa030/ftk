#include "ui/net_panel.hpp"

#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"
#include "net/net_manager.hpp"

#include <imgui.h>

#include <format>
#include <string>

namespace fjell {
namespace {

const char* role_label(NetRole role) {
    switch (role) {
    case NetRole::server: return "Host";
    case NetRole::client: return "Client";
    default:              return "Offline";
    }
}

// One measurement: its name in the label column, the value read out beside it.
void metric(const char* label, const std::string& value) {
    ui::row(label, [&] { ui::readout(value.c_str()); });
}

} // namespace

void NetPanel::draw(float dt, const char* title) {
    auto p = ui::Panel(ui::icon::network, title);
    if (!p) return;

    if (!net_) {
        ui::empty_state(ui::icon::network, "No network manager", "This editor was started without networking");
        return;
    }

    // Per-second rates: the cumulative counters diffed once a second.
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

    if (auto t = ui::PropertyTable("##session")) {
        const NetRole role = net_->role();
        metric("Role", net_->is_connected()
                           ? std::format("{} · peer {}", role_label(role), net_->local_peer())
                           : std::string(role_label(role)));
    }

    if (ui::section_foldable("Bandwidth", ui::icon::bandwidth)) {
        if (auto t = ui::PropertyTable("##bandwidth")) {
            metric("Out", std::format("{:.1f} KB/s ({:.0f} pkt/s)", send_bps_ / 1024.0f, send_pps_));
            metric("In", std::format("{:.1f} KB/s ({:.0f} pkt/s)", recv_bps_ / 1024.0f, recv_pps_));
        }
        ImGui::PlotLines("##bw", send_history_.data(), BANDWIDTH_HISTORY, history_index_,
                         "out B/s", 0.0f, FLT_MAX, {ImGui::GetContentRegionAvail().x, 40});
    }

    if (ui::section_foldable("Peers", ui::icon::peers)) {
        const auto peers = net_->peers();
        if (peers.empty()) {
            ImGui::TextDisabled("No peers connected");
        } else if (ImGui::BeginTable("##peers", 3, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Peer");
            ImGui::TableSetupColumn("RTT");
            ImGui::TableSetupColumn("Loss");
            ImGui::TableHeadersRow();
            for (const auto& peer : peers) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%u", peer.id);
                ImGui::TableNextColumn(); ImGui::Text("%u ms", peer.rtt_ms);
                ImGui::TableNextColumn(); ImGui::Text("%.1f %%", static_cast<double>(peer.loss_pct));
            }
            ImGui::EndTable();
        }
    }

    // Artificial link degradation, so interpolation and prediction can be
    // tried from one machine.
    if (ui::section_foldable("Simulated link", ui::icon::simulated)) {
        if (auto t = ui::PropertyTable("##conditioning")) {
            ui::Edit edit;
            ui::row("Latency", [&] {
                edit |= ui::slider("##latency", sim_latency_ms_, 0.0f, 300.0f, ui::Unit::Milliseconds, "%.0f");
            });
            ui::row("Jitter", [&] {
                edit |= ui::slider("##jitter", sim_jitter_ms_, 0.0f, 100.0f, ui::Unit::Milliseconds, "%.0f");
            });
            ui::row("Loss", [&] {
                edit |= ui::slider("##loss", sim_loss_pct_, 0.0f, 30.0f, ui::Unit::Percent, "%.1f");
            });
            if (!net_->is_connected()) {
                ui::hint("Applies to the live session once connected");
            }
            if (edit.changed) {
                net_->set_conditioning(sim_latency_ms_, sim_jitter_ms_, sim_loss_pct_);
            }
        }
    }
}

} // namespace fjell
