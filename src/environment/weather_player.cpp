#include "environment/weather_player.hpp"

#include "environment/environment_state.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <random>

namespace fjell {

namespace {

// Move `value` toward `target` as a first-order lag with time constant
// `seconds`: most of the way there after that long, never snapping.
float approach(float value, float target, float dt, float seconds) {
    if (seconds <= 0.0f) return target;
    const float k = 1.0f - std::exp(-dt / seconds);
    return value + (target - value) * k;
}

// A steady, deterministic gust: a few sines that never line up, so the wind
// breathes without repeating in a way anyone notices. Returns roughly -1..1.
float gust_noise(float t) {
    return 0.5f * std::sin(t * 1.3f) + 0.3f * std::sin(t * 2.7f + 1.0f) + 0.2f * std::sin(t * 5.1f + 2.0f);
}

glm::vec3 heading_direction(float degrees) {
    const float yaw = glm::radians(degrees);
    return glm::vec3{std::sin(yaw), 0.0f, std::cos(yaw)};
}

// Scale a multiply-track value by how present the event is: absent, the
// multiplier is one.
float weighted_multiplier(float value, float weight) {
    return 1.0f + (value - 1.0f) * weight;
}

glm::vec3 weighted_multiplier(const glm::vec3& value, float weight) {
    return glm::vec3(1.0f) + (value - glm::vec3(1.0f)) * weight;
}

} // namespace

float WeatherPlayer::Playing::progress() const {
    const float duration = durations[static_cast<size_t>(phase)];
    if (duration <= 0.0f) {
        // Indefinite: settle over the phase's minimum, then hold.
        const float settle = std::max(asset != nullptr ? asset->phases[static_cast<size_t>(phase)].min_seconds : 1.0f, 1.0f);
        return std::clamp(phase_time / settle, 0.0f, 1.0f);
    }
    return std::clamp(phase_time / duration, 0.0f, 1.0f);
}

float WeatherPlayer::Playing::phase_units() const {
    return static_cast<float>(phase) + progress();
}

float WeatherPlayer::Playing::weight() const {
    const float p = progress();
    switch (phase) {
        case WeatherPhase::Approach: return p * 0.5f;
        case WeatherPhase::Onset: return 0.5f + p * 0.5f;
        case WeatherPhase::Sustain: return 1.0f;
        case WeatherPhase::Tail: return 1.0f - p;
    }
    return 1.0f;
}

void WeatherPlayer::play(const WeatherAsset* asset, uint32_t event_id, uint32_t seed,
                         const DayProfileAsset* profile, WeatherPhase from) {
    if (asset == nullptr) {
        stop();
        return;
    }
    if (has_active_) {
        // The event so far plays out its tail underneath the new one; an
        // older outgoing event is done for.
        begin_tail(active_);
        outgoing_ = active_;
        has_outgoing_ = true;
    }

    Playing next;
    next.asset = asset;
    next.event_id = event_id;
    next.profile = profile;
    std::mt19937 rng(seed);
    for (uint32_t i = 0; i < WEATHER_PHASE_COUNT; ++i) {
        const PhaseDuration& d = asset->phases[i];
        if (d.indefinite) {
            next.durations[i] = 0.0f;
            continue;
        }
        std::uniform_real_distribution<float> roll(d.min_seconds, std::max(d.max_seconds, d.min_seconds));
        next.durations[i] = std::max(roll(rng), 0.001f);
    }
    next.phase = from;
    active_ = next;
    has_active_ = true;
}

void WeatherPlayer::stop() {
    if (!has_active_) return;
    begin_tail(active_);
    outgoing_ = active_;
    has_outgoing_ = true;
    has_active_ = false;
}

void WeatherPlayer::begin_tail(Playing& p) {
    if (p.phase == WeatherPhase::Tail) return;
    p.phase = WeatherPhase::Tail;
    p.phase_time = 0.0f;
    if (p.durations[static_cast<size_t>(WeatherPhase::Tail)] <= 0.0f && p.asset != nullptr) {
        p.durations[static_cast<size_t>(WeatherPhase::Tail)] =
            std::max(p.asset->phases[static_cast<size_t>(WeatherPhase::Tail)].min_seconds, 0.001f);
    }
}

void WeatherPlayer::advance(Playing& p, float dt) {
    if (p.finished) return;
    p.phase_time += dt;
    while (true) {
        const float duration = p.durations[static_cast<size_t>(p.phase)];
        if (duration <= 0.0f || p.phase_time < duration) return;   // indefinite, or still in the phase
        p.phase_time -= duration;
        if (p.phase == WeatherPhase::Tail) {
            p.finished = true;
            p.phase_time = duration;
            return;
        }
        p.phase = static_cast<WeatherPhase>(static_cast<uint8_t>(p.phase) + 1);
    }
}

void WeatherPlayer::tick(float dt) {
    gust_time_ += dt;
    if (has_outgoing_) {
        advance(outgoing_, dt);
        if (outgoing_.finished) has_outgoing_ = false;
    }
    if (has_active_) {
        advance(active_, dt);
        if (active_.finished) has_active_ = false;
    }
}

WeatherSkyOverride WeatherPlayer::sky_override() const {
    WeatherSkyOverride result;
    if (!has_active_ || active_.asset == nullptr) return result;
    const WeatherAsset& asset = *active_.asset;
    if (active_.profile == nullptr && asset.sky_shader.empty()) return result;
    result.profile = active_.profile;
    result.sky_shader = asset.sky_shader;
    const float blend = asset.tracks.value_of(weather_track::SKY_BLEND, active_.phase_units()).value_or(1.0f);
    result.weight = std::clamp(active_.weight() * blend, 0.0f, 1.0f);
    return result;
}

void WeatherPlayer::strike(const glm::vec3& relative_position, float intensity) {
    last_strike_ = Strike{.position = relative_position, .intensity = intensity,
                          .asset = has_active_ ? active_.asset : nullptr};
    struck_this_tick_ = true;
    flash_time_ = 0.0f;
}

void WeatherPlayer::composite(EnvironmentState& state, float dt, const WeatherRates& rates) {
    WeatherParams& w = state.weather;
    w.params.clear();

    // Wind heading, blended between events by vector so a veer never snaps.
    glm::vec3 wind_dir{0.0f};
    float wind_dir_weight = 0.0f;
    float wind_gust_strength = 0.0f;

    auto fold = [&](const Playing& p) {
        if (p.asset == nullptr) return;
        const WeatherAsset& asset = *p.asset;
        const TrackSet& t = asset.tracks;
        const float x = p.phase_units();
        const float wt = p.weight();
        if (wt <= 0.0f) return;
        using namespace weather_track;

        auto over = [&](std::string_view name, float& target) {
            if (auto v = t.value_of(name, x)) {
                const Track* track = t.find(name);
                if (track->mode == TrackMode::Multiply) target *= weighted_multiplier(*v, wt);
                else target = glm::mix(target, *v, wt);
            }
        };
        auto over_color = [&](std::string_view name, glm::vec3& target) {
            if (auto v = t.color_of(name, x)) {
                const Track* track = t.find(name);
                if (track->mode == TrackMode::Multiply) target *= weighted_multiplier(*v, wt);
                else target = glm::mix(target, *v, wt);
            }
        };

        over(CLOUD_COVERAGE, w.cloud_coverage);
        over(CLOUD_DENSITY, w.cloud_density);
        over(PRECIPITATION, w.precipitation);
        over(WIND_SPEED, w.wind_speed);
        over(LIGHTNING_RATE, w.lightning_rate);
        // The fog's look is its material; the two fog tracks reach the
        // uniforms they always did, faded in by the event's presence.
        if (auto v = t.value_of(FOG_DENSITY, x)) {
            if (auto* u = find_named(state.fog.uniforms, fog_track::DENSITY_UNIFORM)) {
                fold_track(*u, t.find(FOG_DENSITY)->mode, *v, wt);
            }
        }
        if (auto c = t.color_of(FOG_COLOR, x)) {
            if (auto* u = find_named(state.fog.uniforms, fog_track::COLOR_UNIFORM)) {
                fold_track(*u, t.find(FOG_COLOR)->mode, *c, wt);
            }
        }

        // Grouped fog tracks scale the scene's fog volumes, on top of whatever
        // the hour's day profile already made of them: a storm at dawn is
        // thicker than a clear dawn. They ride the same weight as every other
        // track, so an event fading out relaxes its hold rather than dropping it.
        for (const auto& track : t.tracks) {
            const auto density_group =
                fog_group_track::group_of(track.name, fog_group_track::DENSITY_PREFIX);
            if (!density_group.empty()) {
                for (auto& volume : state.fog_volumes) {
                    if (volume.group != density_group) continue;
                    over(track.name, volume.density);
                }
                continue;
            }
            const auto color_group =
                fog_group_track::group_of(track.name, fog_group_track::COLOR_PREFIX);
            if (!color_group.empty()) {
                for (auto& volume : state.fog_volumes) {
                    if (volume.group != color_group) continue;
                    over_color(track.name, volume.color);
                }
            }
        }
        over(SUN_INTENSITY, state.sun.intensity);
        over(AMBIENT_INTENSITY, state.ambient_intensity);
        over_color(SKY_TINT, w.sky_tint);
        if (auto g = t.value_of(WIND_GUST, x)) wind_gust_strength = glm::mix(wind_gust_strength, *g, wt);

        const float veer = t.value_of(WIND_VEER, x).value_or(0.0f);
        wind_dir += heading_direction(asset.wind_heading + veer) * wt;
        wind_dir_weight += wt;

        // Anything else is the event's own value: a sky uniform, a gameplay
        // number, read by name.
        for (const auto& track : t.tracks) {
            if (is_builtin(track.name)) continue;
            NamedValue value;
            value.name = track.name;
            if (track.kind == TrackKind::Color) {
                auto c = t.color_of(track.name, x);
                if (!c) continue;
                value.count = 3;
                value.values = {c->r, c->g, c->b, 0.0f};
            } else {
                auto v = t.value_of(track.name, x);
                if (!v) continue;
                value.count = 1;
                value.values = {*v, 0.0f, 0.0f, 0.0f};
            }
            auto it = std::find_if(w.params.begin(), w.params.end(),
                                   [&](const NamedValue& n) { return n.name == value.name; });
            if (it != w.params.end()) *it = std::move(value);
            else w.params.push_back(std::move(value));
        }
    };

    if (has_outgoing_) fold(outgoing_);
    if (has_active_) fold(active_);

    // Wind: direction from the events, or wherever it blew last; the gust
    // breathes on top of the speed.
    if (wind_dir_weight > 0.0f && glm::length(wind_dir) > 1e-4f) {
        w.wind_direction = glm::normalize(wind_dir);
    }
    w.wind_gust = std::max(1.0f + wind_gust_strength * 0.6f * gust_noise(gust_time_), 0.0f);
    wind_drift_ += glm::vec2(w.wind_direction.x, w.wind_direction.z) * (w.wind_speed * w.wind_gust * dt);
    w.wind_drift = wind_drift_;

    // The front travels with the wind. During the approach its edge sweeps
    // from the windward horizon to the leeward one; through onset and
    // sustain the sky is covered; in the tail the direction flips and the
    // edge sweeps back, opening the sky from windward first.
    const glm::vec2 wind_xz{w.wind_direction.x, w.wind_direction.z};
    glm::vec2 front_dir = glm::length(wind_xz) > 1e-4f ? glm::normalize(wind_xz) : glm::vec2{0.0f, 1.0f};
    float edge = -1.0f - w.front_softness;   // covered everywhere
    const Playing* leading = has_active_ ? &active_ : (has_outgoing_ ? &outgoing_ : nullptr);
    if (leading != nullptr) {
        const float p = leading->progress();
        switch (leading->phase) {
            case WeatherPhase::Approach: edge = glm::mix(1.0f + w.front_softness, -1.0f - w.front_softness, p); break;
            case WeatherPhase::Onset:
            case WeatherPhase::Sustain: edge = -1.0f - w.front_softness; break;
            case WeatherPhase::Tail:
                front_dir = -front_dir;
                edge = glm::mix(-1.0f - w.front_softness, 1.0f + w.front_softness, p);
                break;
        }
    }
    w.front_direction = front_dir;
    w.front_edge = edge;

    // Precipitation type and the surfaces it works on.
    const WeatherAsset* active_asset = has_active_ ? active_.asset : nullptr;
    w.precipitation_type = active_asset != nullptr ? active_asset->precipitation_type : PrecipitationType::None;
    w.precipitation = std::clamp(w.precipitation, 0.0f, 1.0f);
    const bool raining = w.precipitation_type == PrecipitationType::Rain && w.precipitation > 0.0f;
    const bool snowing = w.precipitation_type == PrecipitationType::Snow && w.precipitation > 0.0f;
    const float wet_target = raining && active_asset != nullptr ? active_asset->wetness_target * w.precipitation : 0.0f;
    const float snow_target = snowing && active_asset != nullptr ? active_asset->snow_target * w.precipitation : 0.0f;
    wetness_ = approach(wetness_, wet_target, dt, wet_target > wetness_ ? rates.wetting_seconds : rates.drying_seconds);
    snow_ = approach(snow_, snow_target, dt, snow_target > snow_ ? rates.snowing_seconds : rates.melting_seconds);
    // Rain washes snow away faster than it melts on its own.
    if (raining && snow_ > 0.0f) snow_ = approach(snow_, 0.0f, dt, rates.melting_seconds * 0.25f);
    w.wetness = wetness_;
    w.snow = snow_;

    // ── Lightning ──
    // The rate is strikes a minute, from the event's track, so a storm
    // builds and passes. What a strike looks like is the event's to author.
    struck_this_tick_ = false;
    const WeatherAsset* lightning_asset = has_active_ ? active_.asset : nullptr;
    if (w.lightning_rate > 0.0f && lightning_asset != nullptr) {
        if (strike_countdown_ < 0.0f) strike_countdown_ = 60.0f / w.lightning_rate;
        strike_countdown_ -= dt;
        if (strike_countdown_ <= 0.0f) {
            strike_countdown_ = -1.0f;
            const auto& l = lightning_asset->lightning;
            // Somewhere around the listener, biased toward the far end so
            // most strikes are distant and a close one is an event.
            strike_seed_ = strike_seed_ * 1664525U + 1013904223U;
            const float u1 = static_cast<float>((strike_seed_ >> 8) & 0xFFFFU) / 65535.0f;
            strike_seed_ = strike_seed_ * 1664525U + 1013904223U;
            const float u2 = static_cast<float>((strike_seed_ >> 8) & 0xFFFFU) / 65535.0f;
            const float distance = glm::mix(l.min_distance, l.max_distance, u1 * u1);
            const float bearing = u2 * glm::two_pi<float>();
            strike(glm::vec3(std::sin(bearing) * distance, 0.0f, std::cos(bearing) * distance), 1.0f);
        }
    } else {
        strike_countdown_ = -1.0f;
    }

    // The flash follows the event's curve over its own duration: a stutter
    // if that is what was authored, a single pulse when it was not.
    w.lightning = 0.0f;
    if (flash_time_ >= 0.0f && lightning_asset != nullptr) {
        const auto& l = lightning_asset->lightning;
        flash_time_ += dt;
        const float t = l.duration > 0.0f ? flash_time_ / l.duration : 1.0f;
        if (t >= 1.0f) {
            flash_time_ = -1.0f;
        } else {
            const float shape = l.flash.keyframes.empty()
                // No curve authored: one pulse that fades.
                ? (1.0f - t) * (1.0f - t)
                : l.flash.evaluate(t);
            // Distance dims the flash, so a strike on the horizon lights the
            // sky rather than the scene.
            const float reach = glm::clamp(1.0f - glm::length(last_strike_.position) / std::max(l.max_distance, 1.0f), 0.0f, 1.0f);
            w.lightning = glm::clamp(shape * last_strike_.intensity * glm::mix(0.35f, 1.0f, reach), 0.0f, 1.0f);
        }
    }

    // Playback, for scripts and the inspector.
    if (has_active_) {
        w.event_id = active_.event_id;
        w.phase = active_.phase;
        w.phase_progress = active_.progress();
    } else if (has_outgoing_) {
        w.event_id = outgoing_.event_id;
        w.phase = outgoing_.phase;
        w.phase_progress = outgoing_.progress();
    } else {
        w.event_id = 0;
        w.phase = WeatherPhase::Sustain;
        w.phase_progress = 1.0f;
    }
}

} // namespace fjell
