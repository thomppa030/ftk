#include "environment/environment_scenario.hpp"
#include "ftk/platform/platform.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace fjell;
namespace fs = std::filesystem;

namespace {

fs::path write_scenario(const char* name, const std::string& text) {
    auto dir = fs::temp_directory_path() /
               ("fjell_scenario_test_" + std::to_string(ftk::platform::process_id()));
    fs::create_directories(dir);
    auto path = dir / name;
    std::ofstream(path) << text;
    return path;
}

} // namespace

TEST_CASE("A scenario reads its steps in order", "[scenario]") {
    auto path = write_scenario("basic.fjscenario", R"(
# a storm arrives, then clears
0    hour 13
0    day-length 0
30   weather assets/environment/rain.fjweather approach
150  weather assets/environment/clear.fjweather onset
180  quit
)");

    auto scenario = load_environment_scenario(path.string());
    REQUIRE(scenario);
    REQUIRE(scenario->steps.size() == 5);

    CHECK(scenario->steps[0].time == Catch::Approx(0.0f));
    REQUIRE(scenario->steps[0].change.hour);
    CHECK(*scenario->steps[0].change.hour == Catch::Approx(13.0f));

    REQUIRE(scenario->steps[1].change.day_length);
    CHECK(*scenario->steps[1].change.day_length == Catch::Approx(0.0f));

    REQUIRE(scenario->steps[2].change.weather);
    CHECK(*scenario->steps[2].change.weather == "assets/environment/rain.fjweather");
    REQUIRE(scenario->steps[2].change.weather_phase);
    CHECK(*scenario->steps[2].change.weather_phase == WeatherPhase::Approach);

    REQUIRE(scenario->steps[3].change.weather_phase);
    CHECK(*scenario->steps[3].change.weather_phase == WeatherPhase::Onset);

    CHECK(scenario->steps[4].quit);
    CHECK(scenario->duration() == Catch::Approx(180.0f));
}

TEST_CASE("Steps out of order are sorted, ties keeping their written order", "[scenario]") {
    // Authored order is the reader's business; two steps on one second must
    // still apply as written, since `hour` then `weather` is not the same as
    // the other way about once a weather brings its own profile.
    auto path = write_scenario("order.fjscenario", R"(
90   quit
10   hour 6
0    hour 1
0    day-length 0
)");
    auto scenario = load_environment_scenario(path.string());
    REQUIRE(scenario);
    REQUIRE(scenario->steps.size() == 4);
    CHECK(scenario->steps[0].change.hour);
    CHECK(*scenario->steps[0].change.hour == Catch::Approx(1.0f));
    CHECK(scenario->steps[1].change.day_length);
    CHECK(scenario->steps[2].time == Catch::Approx(10.0f));
    CHECK(scenario->steps[3].quit);
}

TEST_CASE("A weather step's phase is optional", "[scenario]") {
    auto path = write_scenario("nophase.fjscenario",
                               "0 weather assets/environment/rain.fjweather\n");
    auto scenario = load_environment_scenario(path.string());
    REQUIRE(scenario);
    REQUIRE(scenario->steps.size() == 1);
    CHECK(scenario->steps[0].change.weather);
    CHECK_FALSE(scenario->steps[0].change.weather_phase);
}

TEST_CASE("Comments and blank lines carry no steps", "[scenario]") {
    auto path = write_scenario("comments.fjscenario", R"(
# everything here is a comment

   # even indented

0 quit   # and a trailing one
)");
    auto scenario = load_environment_scenario(path.string());
    REQUIRE(scenario);
    REQUIRE(scenario->steps.size() == 1);
    CHECK(scenario->steps[0].quit);
}

TEST_CASE("A bad step is an error naming its line", "[scenario]") {
    // A capture built on a misread script measures the wrong thing without
    // saying so, which is why none of these are skipped.
    SECTION("an unknown command") {
        auto path = write_scenario("bad_cmd.fjscenario", "0 hour 12\n5 rain hard\n");
        auto scenario = load_environment_scenario(path.string());
        REQUIRE_FALSE(scenario);
        CHECK(scenario.error().find(":2:") != std::string::npos);
    }
    SECTION("a time that is not a number") {
        auto path = write_scenario("bad_time.fjscenario", "soon quit\n");
        auto scenario = load_environment_scenario(path.string());
        REQUIRE_FALSE(scenario);
        CHECK(scenario.error().find(":1:") != std::string::npos);
    }
    SECTION("a partly numeric time") {
        auto path = write_scenario("part_time.fjscenario", "12abc quit\n");
        CHECK_FALSE(load_environment_scenario(path.string()));
    }
    SECTION("a phase no phase answers to") {
        auto path = write_scenario("bad_phase.fjscenario",
                                   "0 weather rain.fjweather downpour\n");
        auto scenario = load_environment_scenario(path.string());
        REQUIRE_FALSE(scenario);
        CHECK(scenario.error().find("downpour") != std::string::npos);
    }
    SECTION("a time with no command") {
        CHECK_FALSE(load_environment_scenario(
            write_scenario("no_cmd.fjscenario", "10\n").string()));
    }
    SECTION("a command missing its value") {
        CHECK_FALSE(load_environment_scenario(
            write_scenario("no_value.fjscenario", "0 hour\n").string()));
    }
    SECTION("a negative time") {
        CHECK_FALSE(load_environment_scenario(
            write_scenario("negative.fjscenario", "-5 quit\n").string()));
    }
}

TEST_CASE("A missing scenario file is an error, not an empty scenario", "[scenario]") {
    CHECK_FALSE(load_environment_scenario("/nonexistent/none.fjscenario"));
}

TEST_CASE("Holding and releasing the weather are separate commands", "[scenario]") {
    auto path = write_scenario("hold.fjscenario", "0 hold-weather\n60 release-weather\n");
    auto scenario = load_environment_scenario(path.string());
    REQUIRE(scenario);
    REQUIRE(scenario->steps.size() == 2);
    REQUIRE(scenario->steps[0].change.hold_weather);
    CHECK(*scenario->steps[0].change.hold_weather);
    REQUIRE(scenario->steps[1].change.hold_weather);
    CHECK_FALSE(*scenario->steps[1].change.hold_weather);
}

TEST_CASE("Wetness and snow are set outright, not waited for", "[scenario]") {
    // They integrate from zero over minutes, so a capture that means to
    // measure a wet street has to be able to start on one.
    auto path = write_scenario("wet.fjscenario", "0 wetness 1\n0 snow 0.25\n");
    auto scenario = load_environment_scenario(path.string());
    REQUIRE(scenario);
    REQUIRE(scenario->steps.size() == 2);
    REQUIRE(scenario->steps[0].change.wetness);
    CHECK(*scenario->steps[0].change.wetness == Catch::Approx(1.0f));
    REQUIRE(scenario->steps[1].change.snow);
    CHECK(*scenario->steps[1].change.snow == Catch::Approx(0.25f));

    SECTION("and each needs a number") {
        CHECK_FALSE(load_environment_scenario(
            write_scenario("wet_bad.fjscenario", "0 wetness soaked\n").string()));
        CHECK_FALSE(load_environment_scenario(
            write_scenario("snow_none.fjscenario", "0 snow\n").string()));
    }
}

TEST_CASE("An empty scenario has no steps and no duration", "[scenario]") {
    auto path = write_scenario("empty.fjscenario", "\n# nothing\n\n");
    auto scenario = load_environment_scenario(path.string());
    REQUIRE(scenario);
    CHECK(scenario->steps.empty());
    CHECK(scenario->duration() == Catch::Approx(0.0f));
}

TEST_CASE("Every phase name a scene file or command line may use parses", "[scenario]") {
    for (uint32_t i = 0; i < WEATHER_PHASE_COUNT; ++i) {
        const auto phase = static_cast<WeatherPhase>(i);
        const auto parsed = weather_phase_from_name(weather_phase_name(phase));
        REQUIRE(parsed);
        CHECK(*parsed == phase);
    }
    CHECK_FALSE(weather_phase_from_name(""));
    CHECK_FALSE(weather_phase_from_name("Sustain"));   // names are lower case
}
