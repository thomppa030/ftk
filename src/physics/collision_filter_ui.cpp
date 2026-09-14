#include "physics/collision_filter_ui.hpp"

#include <imgui.h>

namespace fjell {

// Channel picker plus the per-channel response table. Laid out the way people
// think about it — one row per channel, three states — while the filter stores
// it as two masks.
bool draw_collision_filter(CollisionFilter& filter) {
    bool changed = false;

    ImGui::SeparatorText("Collision");

    ImGui::Text("Channel"); ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##Channel", collision_channel_name(filter.channel))) {
        for (uint32_t i = 0; i < COLLISION_CHANNEL_COUNT; ++i) {
            auto channel = static_cast<CollisionChannel>(i);
            bool selected = filter.channel == channel;
            if (ImGui::Selectable(collision_channel_name(channel), selected) && !selected) {
                // Switching channel takes that channel's defaults: the previous
                // table was written against a different meaning.
                filter = default_filter_for(channel);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    if (!ImGui::TreeNodeEx("Responses", ImGuiTreeNodeFlags_DefaultOpen)) return changed;

    // One column per response, one row per channel. Exactly one box is ticked
    // per row — a radio group drawn as checkboxes, which is how a collision
    // matrix reads at a glance. Ignore gets its own box rather than being the
    // absence of the others: "nothing ticked" reads as unset, not as a choice.
    static constexpr struct { CollisionResponse value; const char* label; } COLUMNS[] = {
        {CollisionResponse::ignore, "Ignore"},
        {CollisionResponse::overlap, "Overlap"},
        {CollisionResponse::block, "Block"},
    };

    if (ImGui::BeginTable("responses", 4, ImGuiTableFlags_SizingStretchProp |
                                          ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Channel");
        for (const auto& column : COLUMNS) {
            ImGui::TableSetupColumn(column.label, ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize(column.label).x +
                                        (ImGui::GetFrameHeight() * 1.2F));
        }
        ImGui::TableHeadersRow();

        for (uint32_t i = 0; i < COLLISION_CHANNEL_COUNT; ++i) {
            auto channel = static_cast<CollisionChannel>(i);
            auto current = filter.response_to(channel);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(collision_channel_name(channel));

            ImGui::PushID(static_cast<int>(i));
            for (const auto& column : COLUMNS) {
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(column.value));
                bool ticked = current == column.value;
                // The header names the column, so the box itself is unlabelled.
                // Re-ticking the active box would otherwise clear the row and
                // leave it with no response at all.
                if (ImGui::Checkbox("##r", &ticked) && ticked) {
                    filter.set_response(channel, column.value);
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s %s", column.label,
                                      collision_channel_name(channel));
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TreePop();
    return changed;
}

} // namespace fjell
