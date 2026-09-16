#include "environment/climate_asset.hpp"
#include "environment/climate_director.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <set>

using namespace fjell;
using Catch::Approx;

namespace {

// clear leads to clear or overcast, overcast to anything, rain back to
// overcast: the shape a temperate climate has.
ClimateAsset make_climate() {
    ClimateAsset c;
    c.name = "temperate";
    c.seed = 1337;
    c.initial = "clear";
    c.states = {
        ClimateState{.name = "clear", .event = "clear.fjweather", .weight = 3.0f,
                     .from_hour = 0.0f, .to_hour = 0.0f, .next = {"clear", "overcast"}},
        ClimateState{.name = "overcast", .event = "overcast.fjweather", .weight = 2.0f,
                     .from_hour = 0.0f, .to_hour = 0.0f, .next = {"clear", "overcast", "rain"}},
        ClimateState{.name = "rain", .event = "rain.fjweather", .weight = 1.0f,
                     .from_hour = 0.0f, .to_hour = 0.0f, .next = {"overcast"}},
    };
    return c;
}

} // namespace

TEST_CASE("A state's hour window can wrap midnight", "[climate]") {
    ClimateState any{.name = "a", .event = "a", .weight = 1.0f,
                     .from_hour = 0.0f, .to_hour = 0.0f, .next = {}};
    REQUIRE(any.allows_hour(0.0f));
    REQUIRE(any.allows_hour(13.5f));

    ClimateState day{.name = "d", .event = "d", .weight = 1.0f,
                     .from_hour = 6.0f, .to_hour = 18.0f, .next = {}};
    REQUIRE(day.allows_hour(12.0f));
    REQUIRE(day.allows_hour(6.0f));
    REQUIRE_FALSE(day.allows_hour(18.0f));
    REQUIRE_FALSE(day.allows_hour(3.0f));

    ClimateState night{.name = "n", .event = "n", .weight = 1.0f,
                       .from_hour = 22.0f, .to_hour = 4.0f, .next = {}};
    REQUIRE(night.allows_hour(23.0f));
    REQUIRE(night.allows_hour(1.0f));
    REQUIRE_FALSE(night.allows_hour(12.0f));
    // Hours outside 0..24 wrap onto it.
    REQUIRE(night.allows_hour(25.0f));
}

TEST_CASE("The director starts at the climate's initial state", "[climate]") {
    const ClimateAsset climate = make_climate();
    ClimateDirector director;
    director.set_climate(&climate, 12.0f);

    REQUIRE(director.active());
    REQUIRE(director.current().valid());
    REQUIRE(climate.states[static_cast<size_t>(director.current().state_index)].name == "clear");
    REQUIRE(director.ordinal() == 0);
    REQUIRE(director.forecast().size() == ClimateDirector::FORECAST_LENGTH);

    // No climate: nothing to decide.
    ClimateDirector empty;
    REQUIRE_FALSE(empty.active());
    REQUIRE_FALSE(empty.advance(12.0f).valid());
}

TEST_CASE("Rolls follow the table and repeat for a seed", "[climate]") {
    const ClimateAsset climate = make_climate();
    ClimateDirector a;
    a.set_climate(&climate, 12.0f);

    std::vector<int> first;
    for (int i = 0; i < 20; ++i) {
        const ClimateRoll roll = a.advance(12.0f);
        REQUIRE(roll.valid());
        REQUIRE(roll.ordinal == static_cast<uint32_t>(i + 1));
        first.push_back(roll.state_index);
    }

    // Every step is one the table allows.
    int from = climate.index_of("clear");
    for (int state : first) {
        const auto& allowed = climate.states[static_cast<size_t>(from)].next;
        REQUIRE(std::find(allowed.begin(), allowed.end(),
                          climate.states[static_cast<size_t>(state)].name) != allowed.end());
        from = state;
    }
    // Rain never follows clear directly, and the weighting brings variety.
    REQUIRE(std::set<int>(first.begin(), first.end()).size() > 1);

    // The same seed replays exactly.
    ClimateDirector b;
    b.set_climate(&climate, 12.0f);
    for (int state : first) {
        REQUIRE(b.advance(12.0f).state_index == state);
    }

    // A different seed diverges somewhere in twenty rolls.
    ClimateAsset other = climate;
    other.seed = 99;
    ClimateDirector c;
    c.set_climate(&other, 12.0f);
    std::vector<int> second;
    for (int i = 0; i < 20; ++i) { second.push_back(c.advance(12.0f).state_index); }
    REQUIRE(second != first);
}

TEST_CASE("A client resumes the sequence from an ordinal", "[climate]") {
    const ClimateAsset climate = make_climate();
    ClimateDirector server;
    server.set_climate(&climate, 9.0f);
    for (int i = 0; i < 7; ++i) (void)server.advance(9.0f);

    ClimateDirector client;
    client.resume(&climate, server.ordinal(), 9.0f);
    REQUIRE(client.ordinal() == server.ordinal());
    REQUIRE(client.current().state_index == server.current().state_index);
    REQUIRE(client.forecast().size() == server.forecast().size());
    for (size_t i = 0; i < client.forecast().size(); ++i) {
        REQUIRE(client.forecast()[i].state_index == server.forecast()[i].state_index);
    }
}

TEST_CASE("The forecast is what the next rolls will be", "[climate]") {
    const ClimateAsset climate = make_climate();
    ClimateDirector director;
    director.set_climate(&climate, 12.0f);

    const auto forecast = director.forecast();
    REQUIRE_FALSE(forecast.empty());
    const int predicted = forecast.front().state_index;
    REQUIRE(director.advance(12.0f).state_index == predicted);
}

TEST_CASE("An hour window keeps a state out until its time", "[climate]") {
    ClimateAsset climate;
    climate.seed = 5;
    climate.initial = "day";
    climate.states = {
        ClimateState{.name = "day", .event = "day", .weight = 1.0f,
                     .from_hour = 0.0f, .to_hour = 0.0f, .next = {"day", "fog"}},
        // Morning fog only.
        ClimateState{.name = "fog", .event = "fog", .weight = 5.0f,
                     .from_hour = 4.0f, .to_hour = 8.0f, .next = {"day"}},
    };
    ClimateDirector director;
    director.set_climate(&climate, 12.0f);

    // At noon the heavily weighted fog is out of its window, so the day
    // state is all that can follow.
    for (int i = 0; i < 10; ++i) {
        REQUIRE(climate.states[static_cast<size_t>(director.advance(12.0f).state_index)].name == "day");
    }
    // In its window it wins on weight.
    bool fogged = false;
    for (int i = 0; i < 10 && !fogged; ++i) {
        fogged = climate.states[static_cast<size_t>(director.advance(6.0f).state_index)].name == "fog";
    }
    REQUIRE(fogged);
}

TEST_CASE("Holding freezes the director", "[climate]") {
    const ClimateAsset climate = make_climate();
    ClimateDirector director;
    director.set_climate(&climate, 12.0f);
    const int before = director.current().state_index;

    director.hold(true);
    REQUIRE(director.held());
    REQUIRE_FALSE(director.active());
    REQUIRE_FALSE(director.advance(12.0f).valid());
    REQUIRE(director.current().state_index == before);

    director.hold(false);
    REQUIRE(director.advance(12.0f).valid());
}

TEST_CASE("Climates round-trip through their file", "[climate]") {
    ClimateAsset climate = make_climate();
    climate.states[1].from_hour = 5.0f;
    climate.states[1].to_hour = 9.0f;
    const std::string path = (std::filesystem::temp_directory_path() / "fjell_test.fjclimate").string();
    REQUIRE(save_climate_asset_file(path, climate));

    auto back = load_climate_asset_file(path);
    REQUIRE(back);
    REQUIRE(back->name == "temperate");
    REQUIRE(back->seed == 1337);
    REQUIRE(back->initial == "clear");
    REQUIRE(back->states.size() == 3);
    REQUIRE(back->find("rain") != nullptr);
    REQUIRE(back->find("rain")->event == "rain.fjweather");
    REQUIRE(back->find("rain")->next == std::vector<std::string>{"overcast"});
    REQUIRE(back->find("overcast")->from_hour == Approx(5.0f));
    REQUIRE(back->index_of("missing") == -1);

    // The same table rolls the same weather after a round trip.
    ClimateDirector a;
    ClimateDirector b;
    a.set_climate(&climate, 12.0f);
    b.set_climate(&*back, 12.0f);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(a.advance(12.0f).state_index == b.advance(12.0f).state_index);
    }
}
