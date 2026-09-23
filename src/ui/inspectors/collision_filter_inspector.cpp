#include "ui/inspectors/collision_filter_inspector.hpp"

#include "ui/kit/choice.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"

#include <imgui.h>

#include <iterator>

namespace fjell {

namespace {

constexpr ui::Choice<CollisionChannel> CHANNELS[] = {
    {CollisionChannel::world_static, "World static"},
    {CollisionChannel::world_dynamic, "World dynamic"},
    {CollisionChannel::pawn, "Pawn"},
    {CollisionChannel::trigger, "Trigger"},
    {CollisionChannel::projectile, "Projectile"},
};
static_assert(std::size(CHANNELS) == COLLISION_CHANNEL_COUNT);

constexpr ui::Choice<CollisionResponse> RESPONSES[] = {
    {CollisionResponse::ignore, "Ignore"},
    {CollisionResponse::overlap, "Overlap"},
    {CollisionResponse::block, "Block"},
};

} // namespace

ui::Edit draw_collision_filter(CollisionFilter& filter) {
    ui::Edit edit;
    ui::subheading("Collision");
    if (auto t = ui::PropertyTable("##collision")) {
        ui::row("Channel", "What this collider is to everything else. Switching it starts from "
                           "that channel's usual responses.",
                [&] {
                    CollisionChannel channel = filter.channel;
                    const ui::Edit e = ui::choice("##channel", channel, CHANNELS);
                    // The previous table was written against another meaning.
                    if (e.changed) filter = default_filter_for(channel);
                    edit |= e;
                });
    }

    ui::subheading("Responses");
    if (auto t = ui::PropertyTable("##responses")) {
        for (const auto& [channel, name] : CHANNELS) {
            ImGui::PushID(static_cast<int>(channel));
            ui::row(name, [&] {
                CollisionResponse response = filter.response_to(channel);
                const ui::Edit e = ui::choice("##response", response, RESPONSES);
                if (e.changed) filter.set_response(channel, response);
                edit |= e;
            });
            ImGui::PopID();
        }
    }
    return edit;
}

} // namespace fjell
