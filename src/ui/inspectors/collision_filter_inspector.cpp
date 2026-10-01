#include "ui/inspectors/collision_filter_inspector.hpp"

#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"

#include <imgui.h>

#include <iterator>

namespace fjell {

namespace {

constexpr ftk::ui::Choice<CollisionChannel> CHANNELS[] = {
    {CollisionChannel::world_static, "World static"},
    {CollisionChannel::world_dynamic, "World dynamic"},
    {CollisionChannel::pawn, "Pawn"},
    {CollisionChannel::trigger, "Trigger"},
    {CollisionChannel::projectile, "Projectile"},
};
static_assert(std::size(CHANNELS) == COLLISION_CHANNEL_COUNT);

constexpr ftk::ui::Choice<CollisionResponse> RESPONSES[] = {
    {CollisionResponse::ignore, "Ignore"},
    {CollisionResponse::overlap, "Overlap"},
    {CollisionResponse::block, "Block"},
};

} // namespace

ftk::ui::Edit draw_collision_filter(CollisionFilter& filter) {
    ftk::ui::Edit edit;
    ftk::ui::subheading("Collision");
    if (auto t = ftk::ui::PropertyTable("##collision")) {
        ftk::ui::row("Channel", "What this collider is to everything else. Switching it starts from "
                           "that channel's usual responses.",
                [&] {
                    CollisionChannel channel = filter.channel;
                    const ftk::ui::Edit e = ftk::ui::choice("##channel", channel, CHANNELS);
                    // The previous table was written against another meaning.
                    if (e.changed) filter = default_filter_for(channel);
                    edit |= e;
                });
    }

    ftk::ui::subheading("Responses");
    if (auto t = ftk::ui::PropertyTable("##responses")) {
        for (const auto& [channel, name] : CHANNELS) {
            ImGui::PushID(static_cast<int>(channel));
            ftk::ui::row(name, [&] {
                CollisionResponse response = filter.response_to(channel);
                const ftk::ui::Edit e = ftk::ui::choice("##response", response, RESPONSES);
                if (e.changed) filter.set_response(channel, response);
                edit |= e;
            });
            ImGui::PopID();
        }
    }
    return edit;
}

} // namespace fjell
