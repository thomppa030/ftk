#include <catch2/catch_test_macros.hpp>

#include "physics/collision_channel.hpp"

using namespace fjell;

namespace {

CollisionFilter filter_on(CollisionChannel channel) {
    CollisionFilter f;
    f.channel = channel;
    f.block_mask = 0;
    f.overlap_mask = 0;
    return f;
}

} // namespace

TEST_CASE("Response masks round-trip through set_response", "[collision]") {
    auto f = filter_on(CollisionChannel::pawn);

    f.set_response(CollisionChannel::world_static, CollisionResponse::block);
    f.set_response(CollisionChannel::trigger, CollisionResponse::overlap);
    f.set_response(CollisionChannel::projectile, CollisionResponse::ignore);

    REQUIRE(f.response_to(CollisionChannel::world_static) == CollisionResponse::block);
    REQUIRE(f.response_to(CollisionChannel::trigger) == CollisionResponse::overlap);
    REQUIRE(f.response_to(CollisionChannel::projectile) == CollisionResponse::ignore);
}

TEST_CASE("Setting a response clears the previous one", "[collision]") {
    auto f = filter_on(CollisionChannel::pawn);

    f.set_response(CollisionChannel::pawn, CollisionResponse::block);
    f.set_response(CollisionChannel::pawn, CollisionResponse::overlap);

    // Both masks are separate bits; the old one has to be cleared or the
    // stronger response would keep winning.
    REQUIRE(f.response_to(CollisionChannel::pawn) == CollisionResponse::overlap);

    f.set_response(CollisionChannel::pawn, CollisionResponse::ignore);
    REQUIRE(f.response_to(CollisionChannel::pawn) == CollisionResponse::ignore);
}

TEST_CASE("The weaker response decides the pair", "[collision]") {
    auto pawn = filter_on(CollisionChannel::pawn);
    auto wall = filter_on(CollisionChannel::world_static);

    SECTION("both blocking blocks") {
        pawn.set_response(CollisionChannel::world_static, CollisionResponse::block);
        wall.set_response(CollisionChannel::pawn, CollisionResponse::block);
        REQUIRE(collision_response(pawn, wall) == CollisionResponse::block);
    }

    SECTION("one overlapping downgrades the pair to overlap") {
        pawn.set_response(CollisionChannel::world_static, CollisionResponse::overlap);
        wall.set_response(CollisionChannel::pawn, CollisionResponse::block);
        REQUIRE(collision_response(pawn, wall) == CollisionResponse::overlap);
    }

    SECTION("either side ignoring ends it") {
        pawn.set_response(CollisionChannel::world_static, CollisionResponse::ignore);
        wall.set_response(CollisionChannel::pawn, CollisionResponse::block);
        REQUIRE(collision_response(pawn, wall) == CollisionResponse::ignore);
    }

    SECTION("the rule is symmetric") {
        pawn.set_response(CollisionChannel::world_static, CollisionResponse::overlap);
        wall.set_response(CollisionChannel::pawn, CollisionResponse::ignore);
        REQUIRE(collision_response(pawn, wall) == collision_response(wall, pawn));
        REQUIRE(collision_response(pawn, wall) == CollisionResponse::ignore);
    }
}

TEST_CASE("Defaults keep the pre-channel behaviour", "[collision]") {
    auto pawn = default_filter_for(CollisionChannel::pawn);
    auto wall = default_filter_for(CollisionChannel::world_static);
    auto prop = default_filter_for(CollisionChannel::world_dynamic);

    // A character walking into level geometry is the case that must not need
    // configuring to work.
    REQUIRE(collision_response(pawn, wall) == CollisionResponse::block);
    REQUIRE(collision_response(pawn, prop) == CollisionResponse::block);
    REQUIRE(collision_response(prop, wall) == CollisionResponse::block);
}

TEST_CASE("Defaults that would otherwise be bugs", "[collision]") {
    auto trigger = default_filter_for(CollisionChannel::trigger);
    auto pawn = default_filter_for(CollisionChannel::pawn);
    auto wall = default_filter_for(CollisionChannel::world_static);
    auto shot = default_filter_for(CollisionChannel::projectile);

    SECTION("a trigger detects rather than blocks") {
        REQUIRE(collision_response(trigger, pawn) == CollisionResponse::overlap);
    }

    SECTION("two triggers do not report each other") {
        auto other = default_filter_for(CollisionChannel::trigger);
        REQUIRE(collision_response(trigger, other) == CollisionResponse::ignore);
    }

    SECTION("static geometry does not collide with itself") {
        auto other = default_filter_for(CollisionChannel::world_static);
        REQUIRE(collision_response(wall, other) == CollisionResponse::ignore);
    }

    SECTION("projectiles pass each other but hit the world") {
        auto other = default_filter_for(CollisionChannel::projectile);
        REQUIRE(collision_response(shot, other) == CollisionResponse::ignore);
        REQUIRE(collision_response(shot, wall) == CollisionResponse::block);
    }
}

TEST_CASE("Every channel has a name", "[collision]") {
    for (uint32_t i = 0; i < COLLISION_CHANNEL_COUNT; ++i) {
        auto name = collision_channel_name(static_cast<CollisionChannel>(i));
        REQUIRE(name != nullptr);
        REQUIRE(std::string(name) != "Unknown");
    }
}
