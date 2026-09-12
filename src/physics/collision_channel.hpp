#pragma once

// What collides with what. Engine-side data and an engine-side rule: the
// physics backend asks this whether a pair interacts, it does not decide for
// itself. Filtering is where a backend's own layer model is most tempting to
// adopt wholesale, and adopting it would make the backend load-bearing for
// something that is purely gameplay semantics.

#include <cstdint>

namespace fjell {

// The channels a collider can belong to. Fixed for now; the response table and
// the rule below are indexed by channel rather than hardcoded per name, so
// project-defined channels later become a question of where the names come
// from rather than a change to how any of this works.
enum class CollisionChannel : uint8_t {
    world_static = 0,   // level geometry that never moves
    world_dynamic = 1,  // props that move but aren't characters
    pawn = 2,           // characters, player and AI
    trigger = 3,        // volumes that detect rather than block
    projectile = 4,
};

static constexpr uint32_t COLLISION_CHANNEL_COUNT = 5;

[[nodiscard]] const char* collision_channel_name(CollisionChannel channel);

// How a collider reacts to another channel. Two masks express the whole table:
// Block is "in block_mask", Overlap is "in overlap_mask", Ignore is neither.
// That keeps the per-pair test to a couple of bit operations while the UI
// presents the three states people actually think in.
enum class CollisionResponse : uint8_t {
    ignore = 0,   // pass through, no event
    overlap = 1,  // no physical response, but report the contact
    block = 2,    // collide normally
};

// A collider's channel and what it does about every other channel.
struct CollisionFilter {
    CollisionChannel channel{CollisionChannel::world_static};

    // Bit N is set when this collider blocks/overlaps channel N.
    uint32_t block_mask{0xFFFFFFFF};
    uint32_t overlap_mask{0};

    [[nodiscard]] CollisionResponse response_to(CollisionChannel other) const {
        const uint32_t bit = 1U << static_cast<uint32_t>(other);
        if ((block_mask & bit) != 0) return CollisionResponse::block;
        if ((overlap_mask & bit) != 0) return CollisionResponse::overlap;
        return CollisionResponse::ignore;
    }

    void set_response(CollisionChannel other, CollisionResponse response);
};

// What actually happens when two colliders meet.
//
// The weaker of the two responses wins: blocking is something both sides have
// to agree on, so a collider that merely overlaps a channel is not made solid
// by the other side wanting to block it. Either side ignoring the other ends
// it entirely — that is what "ignore" has to mean for a filter to be usable as
// an opt-out.
[[nodiscard]] CollisionResponse collision_response(const CollisionFilter& a,
                                                   const CollisionFilter& b);

// The default response table for a channel — what a freshly added collider on
// that channel does before anyone edits it.
[[nodiscard]] CollisionFilter default_filter_for(CollisionChannel channel);

} // namespace fjell
