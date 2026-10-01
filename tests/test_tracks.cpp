#include "environment/color_gradient.hpp"
#include "environment/day_profile_asset.hpp"
#include "environment/environment_state.hpp"
#include "environment/material_track.hpp"
#include "environment/track_set.hpp"
#include "ftk/math/color_space.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

using namespace fjell;
using Catch::Approx;

namespace {

bool close(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(a - b) < 1e-4f;
}

} // namespace

TEST_CASE("A colour gradient blends in linear light and can wrap", "[tracks]") {
    ColorGradient g;
    g.keys = {{0.25f, glm::vec3(0.0f)}, {0.75f, glm::vec3(1.0f)}};

    REQUIRE(close(g.evaluate(0.5f), glm::vec3(0.5f)));
    REQUIRE(close(g.evaluate(0.0f), glm::vec3(0.0f)));   // holds the first stop
    REQUIRE(close(g.evaluate(1.0f), glm::vec3(1.0f)));   // holds the last stop

    // Wrapped, the gap from 0.75 back around to 0.25 is one half-period blend.
    REQUIRE(close(g.evaluate(0.0f, true), glm::vec3(0.5f)));
    REQUIRE(close(g.evaluate(0.875f, true), glm::vec3(0.75f)));
    REQUIRE(close(g.evaluate(0.125f, true), glm::vec3(0.25f)));

    // Stops sort themselves and survive the sRGB file round trip.
    ColorGradient unsorted;
    unsorted.keys = {{0.8f, glm::vec3(1.0f, 0.0f, 0.0f)}, {0.2f, glm::vec3(0.0f, 0.0f, 1.0f)}};
    nlohmann::json j;
    unsorted.to_json(j);
    ColorGradient back;
    back.from_json(j);
    REQUIRE(back.keys.size() == 2);
    REQUIRE(back.keys[0].time == Approx(0.2f));
    REQUIRE(close(back.keys[0].color, glm::vec3(0.0f, 0.0f, 1.0f)));
    REQUIRE(close(back.keys[1].color, glm::vec3(1.0f, 0.0f, 0.0f)));
}

TEST_CASE("A track set evaluates in its own domain and folds by mode", "[tracks]") {
    TrackSet set;
    set.length = 24.0f;
    set.wrap = true;

    auto& intensity = set.add("sun_intensity", TrackKind::Float, TrackMode::Multiply);
    intensity.curve.keyframes = {{6.0f / 24.0f, 0.0f, 0.0f, 0.0f},
                                 {12.0f / 24.0f, 1.0f, 0.0f, 0.0f},
                                 {18.0f / 24.0f, 0.0f, 0.0f, 0.0f}};
    auto& tint = set.add("sun_color", TrackKind::Color, TrackMode::Override);
    tint.gradient.keys = {{6.0f / 24.0f, glm::vec3(1.0f, 0.5f, 0.2f)},
                          {12.0f / 24.0f, glm::vec3(1.0f)}};

    REQUIRE(*set.value_of("sun_intensity", 12.0f) == Approx(1.0f));
    REQUIRE(*set.value_of("sun_intensity", 6.0f) == Approx(0.0f));
    REQUIRE_FALSE(set.value_of("missing", 12.0f).has_value());
    REQUIRE_FALSE(set.value_of("sun_color", 12.0f).has_value());   // wrong kind

    // Wrap: past 18 the curve heads back toward the 6 o'clock key through
    // midnight rather than holding the last value.
    REQUIRE(*set.value_of("sun_intensity", 24.0f) == Approx(0.0f).margin(1e-4));
    REQUIRE(*set.value_of("sun_intensity", 30.0f) == Approx(*set.value_of("sun_intensity", 6.0f)).margin(1e-4));

    float target = 2.0f;
    set.apply("sun_intensity", 12.0f, target);
    REQUIRE(target == Approx(2.0f));           // multiply by one
    set.apply("sun_intensity", 9.0f, target);
    REQUIRE(target < 2.0f);
    set.apply("missing", 9.0f, target);        // absent track leaves it alone
    REQUIRE(target < 2.0f);

    glm::vec3 color(0.3f);
    set.apply("sun_color", 12.0f, color);
    REQUIRE(close(color, glm::vec3(1.0f)));    // override replaces

    // Round trip through JSON keeps keys in domain units and the modes.
    nlohmann::json j;
    set.to_json(j);
    REQUIRE(j["tracks"][0]["keys"][1]["t"].get<float>() == Approx(12.0f));
    TrackSet back;
    back.from_json(j);
    REQUIRE(back.length == Approx(24.0f));
    REQUIRE(back.wrap);
    REQUIRE(back.find("sun_intensity")->mode == TrackMode::Multiply);
    REQUIRE(back.find("sun_color")->kind == TrackKind::Color);
    REQUIRE(*back.value_of("sun_intensity", 12.0f) == Approx(1.0f));
    REQUIRE(*back.value_of("sun_intensity", 9.0f) == Approx(*set.value_of("sun_intensity", 9.0f)).margin(1e-4));
}

TEST_CASE("A day profile folds into the environment and leaves the rest alone", "[tracks]") {
    DayProfileAsset profile;
    profile.tracks.add(std::string(day_track::SUN_INTENSITY), TrackKind::Float, TrackMode::Multiply)
        .curve.keyframes = {{0.5f, 0.5f, 0.0f, 0.0f}};
    profile.tracks.add(std::string(day_track::FOG_COLOR), TrackKind::Color, TrackMode::Override)
        .gradient.keys = {{0.5f, glm::vec3(0.2f, 0.3f, 0.4f)}};
    profile.tracks.add(std::string(day_track::EXPOSURE_BIAS), TrackKind::Float, TrackMode::Override)
        .curve.keyframes = {{0.5f, -1.5f, 0.0f, 0.0f}};
    profile.tracks.add("haze", TrackKind::Float, TrackMode::Override)
        .curve.keyframes = {{0.5f, 0.7f, 0.0f, 0.0f}};

    EnvironmentState state;
    state.sun.intensity = 2.0f;
    state.sun.color = glm::vec3(1.0f, 0.9f, 0.8f);
    state.ambient_intensity = 0.4f;
    state.fog.uniforms = {NamedValue{.name = "color", .count = 3,
                                     .values = {0.9f, 0.9f, 0.9f}, .srgb = true}};
    profile.apply(12.0f, state);

    REQUIRE(state.sun.intensity == Approx(1.0f));
    REQUIRE(close(state.sun.color, glm::vec3(1.0f, 0.9f, 0.8f)));   // no track: unchanged
    REQUIRE(state.ambient_intensity == Approx(0.4f));
    // The gradient is linear light; the fog's colour uniform holds sRGB, so
    // the track lands converted and reads back as the colour it set.
    const NamedValue* fog_color = find_named(state.fog.uniforms, "color");
    REQUIRE(fog_color != nullptr);
    REQUIRE(close(color_space::srgb_to_linear(glm::vec3(fog_color->values[0], fog_color->values[1],
                                                        fog_color->values[2])),
                  glm::vec3(0.2f, 0.3f, 0.4f)));
    REQUIRE(state.exposure_bias == Approx(-1.5f));
    REQUIRE(state.sky_uniforms.size() == 1);
    REQUIRE(state.sky_uniforms[0].name == "haze");
    REQUIRE(state.sky_uniforms[0].count == 1);
    REQUIRE(state.sky_uniforms[0].values[0] == Approx(0.7f));

    REQUIRE(day_track::kind_of(day_track::SUN_COLOR) == TrackKind::Color);
    REQUIRE(day_track::kind_of(day_track::SUN_INTENSITY) == TrackKind::Float);
    REQUIRE(day_track::is_builtin(day_track::STAR_INTENSITY));
    REQUIRE_FALSE(day_track::is_builtin("haze"));
}

TEST_CASE("A fog track names a uniform of the haze or of a group", "[tracks][fog]") {
    using material_track::parse;
    using material_track::Kind;
    auto t = parse("fog.density");
    REQUIRE(t);
    REQUIRE(t->kind == Kind::Fog);
    REQUIRE(t->group.empty());
    REQUIRE(t->uniform == "density");
    t = parse("fog.valley.noise_contrast");
    REQUIRE(t);
    REQUIRE(t->group == "valley");
    REQUIRE(t->uniform == "noise_contrast");
    // A group may carry dots of its own; the uniform never does.
    t = parse("fog.river.lower.density");
    REQUIRE(t);
    REQUIRE(t->group == "river.lower");
    REQUIRE(t->uniform == "density");
    // The four names from before the fog had a material.
    REQUIRE(parse("fog_density")->uniform == "density");
    REQUIRE(parse("fog_density")->group.empty());
    REQUIRE(parse("fog_color")->uniform == "color");
    REQUIRE(parse("fog_density.valley")->group == "valley");
    REQUIRE(parse("fog_color.valley")->uniform == "color");
    // Not fog tracks: they fall through to the sky uniforms.
    REQUIRE_FALSE(parse("haze"));
    REQUIRE_FALSE(parse("fog"));
    REQUIRE_FALSE(parse("fog."));
    REQUIRE_FALSE(parse("fog..x"));
    REQUIRE_FALSE(parse("foggy.density"));
    REQUIRE(material_track::name_for(Kind::Fog, "", "density") == "fog.density");
    REQUIRE(material_track::name_for(Kind::Fog, "valley", "color") == "fog.valley.color");
}

TEST_CASE("A post track names a uniform of the infinite volumes or of a group", "[tracks][post]") {
    using material_track::parse;
    using material_track::Kind;
    auto t = parse("post.saturation");
    REQUIRE(t);
    REQUIRE(t->kind == Kind::Post);
    REQUIRE(t->group.empty());
    REQUIRE(t->uniform == "saturation");
    t = parse("post.interior.grade_amount");
    REQUIRE(t);
    REQUIRE(t->kind == Kind::Post);
    REQUIRE(t->group == "interior");
    REQUIRE(t->uniform == "grade_amount");
    REQUIRE_FALSE(parse("post"));
    REQUIRE_FALSE(parse("post."));
    REQUIRE_FALSE(parse("postal.x"));
    REQUIRE(material_track::name_for(Kind::Post, "", "saturation") == "post.saturation");
    REQUIRE(material_track::name_for(Kind::Post, "cave", "exposure") == "post.cave.exposure");
    REQUIRE(material_track::heading(*parse("post.cave.exposure")) == "Post \xc2\xb7 cave");
    REQUIRE(material_track::heading(*parse("fog.density")) == "Fog");
}

TEST_CASE("A post track moves the base look, or only its own group", "[tracks][post]") {
    DayProfileAsset profile;
    profile.tracks.add("post.saturation", TrackKind::Float, TrackMode::Override)
        .curve.keyframes = {{0.5f, 0.25f, 0.0f, 0.0f}};
    profile.tracks.add("post.interior.bloom_intensity", TrackKind::Float, TrackMode::Multiply)
        .curve.keyframes = {{0.5f, 2.0f, 0.0f, 0.0f}};

    auto volume = [](bool infinite, const char* group) {
        return PostVolume{.infinite = infinite,
                          .group = group,
                          .uniforms = {NamedValue{.name = "saturation", .count = 1, .values = {1.0f}},
                                       NamedValue{.name = "bloom_intensity", .count = 1,
                                                  .values = {0.3f}}}};
    };
    EnvironmentState state;
    state.post_volumes.push_back(volume(true, ""));          // the base look
    state.post_volumes.push_back(volume(false, ""));         // a room, ungrouped
    state.post_volumes.push_back(volume(false, "interior")); // a room in the group
    profile.apply(12.0f, state);

    auto value = [&](size_t i, const char* name) {
        return find_named(state.post_volumes[i].uniforms, name)->values[0];
    };
    // The ungrouped track is the base look's alone.
    REQUIRE(value(0, "saturation") == Approx(0.25f));
    REQUIRE(value(1, "saturation") == Approx(1.0f));
    REQUIRE(value(2, "saturation") == Approx(1.0f));
    // The grouped one reaches its group, bounded or not.
    REQUIRE(value(0, "bloom_intensity") == Approx(0.3f));
    REQUIRE(value(2, "bloom_intensity") == Approx(0.6f));
    // Neither is a sky uniform.
    REQUIRE(state.sky_uniforms.empty());
}

TEST_CASE("A fog track drives any uniform the material declares", "[tracks][fog]") {
    DayProfileAsset profile;
    profile.tracks.add("fog.density", TrackKind::Float, TrackMode::Multiply)
        .curve.keyframes = {{0.5f, 2.0f, 0.0f, 0.0f}};
    profile.tracks.add("fog.valley.noise_contrast", TrackKind::Float, TrackMode::Override)
        .curve.keyframes = {{0.5f, 0.8f, 0.0f, 0.0f}};
    profile.tracks.add("fog.valley.color", TrackKind::Color, TrackMode::Override)
        .gradient.keys = {{0.5f, glm::vec3(0.9f, 0.5f, 0.2f)}};
    profile.tracks.add("fog.nowhere", TrackKind::Float, TrackMode::Override)
        .curve.keyframes = {{0.5f, 7.0f, 0.0f, 0.0f}};

    EnvironmentState state;
    state.fog.uniforms = {NamedValue{.name = "density", .count = 1, .values = {0.01f}}};
    state.fog_volumes.push_back(FogVolume{
        .group = "valley",
        .uniforms = {NamedValue{.name = "noise_contrast", .count = 1, .values = {0.0f}},
                     NamedValue{.name = "color", .count = 3, .values = {0.5f, 0.5f, 0.5f},
                                .srgb = true}}});
    state.fog_volumes.push_back(FogVolume{
        .group = "cave",
        .uniforms = {NamedValue{.name = "noise_contrast", .count = 1, .values = {0.0f}}}});
    profile.apply(12.0f, state);

    REQUIRE(state.fog.scalar("density", 0.0f) == Approx(0.02f));
    REQUIRE(find_named(state.fog_volumes[0].uniforms, "noise_contrast")->values[0] == Approx(0.8f));
    REQUIRE(find_named(state.fog_volumes[1].uniforms, "noise_contrast")->values[0] == Approx(0.0f));
    const NamedValue* c = find_named(state.fog_volumes[0].uniforms, "color");
    REQUIRE(close(color_space::srgb_to_linear(glm::vec3(c->values[0], c->values[1], c->values[2])),
                  glm::vec3(0.9f, 0.5f, 0.2f)));
    // A uniform nothing declares reaches nothing, and is not a sky uniform.
    REQUIRE(find_named(state.fog.uniforms, "nowhere") == nullptr);
    REQUIRE(state.sky_uniforms.empty());
}

TEST_CASE("A grouped fog track moves only its own volumes", "[tracks]") {
    DayProfileAsset profile;
    Track& valley = profile.tracks.add("fog_density.valley", TrackKind::Float, TrackMode::Multiply);
    valley.curve.keyframes = {{0.5f, 3.0f, 0.0f, 0.0f}};
    Track& tint = profile.tracks.add("fog_color.valley", TrackKind::Color, TrackMode::Override);
    tint.gradient.keys = {{0.5f, glm::vec3(0.9f, 0.5f, 0.2f)}};
    Track& global = profile.tracks.add("fog_density", TrackKind::Float, TrackMode::Multiply);
    global.curve.keyframes = {{0.5f, 2.0f, 0.0f, 0.0f}};

    EnvironmentState state;
    state.fog.uniforms = {NamedValue{.name = "density", .count = 1, .values = {0.01f}}};
    auto bank = [](const char* group) {
        return FogVolume{.group = group,
                         .uniforms = {NamedValue{.name = "density", .count = 1, .values = {0.05f}},
                                      NamedValue{.name = "color", .count = 3,
                                                 .values = {0.5f, 0.5f, 0.5f}, .srgb = true}}};
    };
    state.fog_volumes.push_back(bank("valley"));
    state.fog_volumes.push_back(bank("cave"));
    state.fog_volumes.push_back(bank(""));
    auto density_of = [&](size_t i) {
        return find_named(state.fog_volumes[i].uniforms, "density")->values[0];
    };
    auto color_of = [&](size_t i) {
        const NamedValue* c = find_named(state.fog_volumes[i].uniforms, "color");
        return color_space::srgb_to_linear(glm::vec3(c->values[0], c->values[1], c->values[2]));
    };
    const glm::vec3 authored_color = color_of(1);

    profile.apply(12.0f, state);

    // The ungrouped track is the global fog's alone; it does not reach volumes.
    REQUIRE(state.fog.scalar("density", 0.0f) == Approx(0.02f));
    REQUIRE(density_of(0) == Approx(0.15f));   // tagged: scaled
    REQUIRE(density_of(1) == Approx(0.05f));   // another group
    REQUIRE(density_of(2) == Approx(0.05f));   // untagged

    REQUIRE(close(color_of(0), glm::vec3(0.9f, 0.5f, 0.2f)));
    REQUIRE(close(color_of(1), authored_color));

    // A grouped track is not a sky uniform, however unknown its name looks.
    REQUIRE(state.sky_uniforms.empty());
}
