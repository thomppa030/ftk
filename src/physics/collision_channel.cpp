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

bool collision_channel_from_name(std::string_view name, CollisionChannel& out) {
    for (uint32_t i = 0; i < COLLISION_CHANNEL_COUNT; ++i) {
        auto candidate = static_cast<CollisionChannel>(i);
        if (name == collision_channel_name(candidate)) {
            out = candidate;
            return true;
        }
    }
    return false;
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
    //
    // "Static geometry never collides with itself" is not one of them, however
    // true it sounds. A collider nobody has configured is WorldStatic, and so
    // is terrain, so writing that as a response makes every default collider
    // fall through the ground the moment it is given a dynamic body. Bodies
    // that genuinely cannot move are dropped by the broadphase, which knows
    // motion type and does not have to guess it from a channel.
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

        default:
            break;
    }
    return filter;
}

} // namespace fjell
