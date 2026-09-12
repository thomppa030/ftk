#include "physics/collision_channel.hpp"

namespace fjell {

const char* collision_channel_name(CollisionChannel channel) {
    switch (channel) {
        case CollisionChannel::world_static: return "World Static";
        case CollisionChannel::world_dynamic: return "World Dynamic";
        case CollisionChannel::pawn: return "Pawn";
        case CollisionChannel::trigger: return "Trigger";
        case CollisionChannel::projectile: return "Projectile";
    }
    return "Unknown";
}

void CollisionFilter::set_response(CollisionChannel other, CollisionResponse response) {
    const uint32_t bit = 1U << static_cast<uint32_t>(other);
    block_mask &= ~bit;
    overlap_mask &= ~bit;
    switch (response) {
        case CollisionResponse::block:   block_mask |= bit; break;
        case CollisionResponse::overlap: overlap_mask |= bit; break;
        case CollisionResponse::ignore:  break;
    }
}

CollisionResponse collision_response(const CollisionFilter& a, const CollisionFilter& b) {
    auto a_to_b = a.response_to(b.channel);
    auto b_to_a = b.response_to(a.channel);
    return a_to_b < b_to_a ? a_to_b : b_to_a;
}

CollisionFilter default_filter_for(CollisionChannel channel) {
    CollisionFilter filter;
    filter.channel = channel;

    // Everything blocks everything by default, which is the behaviour before
    // channels existed. Departures below are the ones that would otherwise be
    // immediate bugs rather than preferences.
    filter.block_mask = 0;
    for (uint32_t i = 0; i < COLLISION_CHANNEL_COUNT; ++i) {
        filter.set_response(static_cast<CollisionChannel>(i), CollisionResponse::block);
    }

    switch (channel) {
        case CollisionChannel::trigger:
            // A trigger that blocks is a wall, which is never what one is for.
            for (uint32_t i = 0; i < COLLISION_CHANNEL_COUNT; ++i) {
                filter.set_response(static_cast<CollisionChannel>(i),
                                    CollisionResponse::overlap);
            }
            // Two triggers overlapping each other is noise, not an event.
            filter.set_response(CollisionChannel::trigger, CollisionResponse::ignore);
            break;

        case CollisionChannel::projectile:
            // Projectiles pass each other rather than deflecting mid-flight.
            filter.set_response(CollisionChannel::projectile, CollisionResponse::ignore);
            break;

        case CollisionChannel::world_static:
            // Static geometry never moves, so a pair of it can never collide.
            filter.set_response(CollisionChannel::world_static, CollisionResponse::ignore);
            break;

        default:
            break;
    }
    return filter;
}

} // namespace fjell
