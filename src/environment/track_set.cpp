#include "environment/track_set.hpp"

#include "core/math/color_space.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace fjell {

namespace {

float hermite(float v0, float m0, float v1, float m1, float u) {
    const float u2 = u * u;
    const float u3 = u2 * u;
    const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    const float h10 = u3 - 2.0f * u2 + u;
    const float h01 = -2.0f * u3 + 3.0f * u2;
    const float h11 = u3 - u2;
    return h00 * v0 + h10 * m0 + h01 * v1 + h11 * m1;
}

float normalize_time(float x, float length, bool wrap) {
    if (length <= 0.0f) return 0.0f;
    float t = x / length;
    if (wrap) {
        t = std::fmod(t, 1.0f);
        if (t < 0.0f) t += 1.0f;
    }
    return std::clamp(t, 0.0f, 1.0f);
}

const char* mode_name(TrackMode mode) {
    return mode == TrackMode::Multiply ? "multiply" : "override";
}

TrackMode mode_from(const std::string& name) {
    return name == "multiply" ? TrackMode::Multiply : TrackMode::Override;
}

} // namespace

float evaluate_periodic(const Curve& curve, float t) {
    const auto& keys = curve.keyframes;
    if (keys.size() < 2) return curve.evaluate(t);
    t = std::clamp(t, 0.0f, 1.0f);
    const auto& first = keys.front();
    const auto& last = keys.back();
    if (t >= first.time && t <= last.time) return curve.evaluate(t);

    // Across the seam: from the last key up to the first key one period on.
    const float gap = (1.0f - last.time) + first.time;
    if (gap <= 1e-6f) return first.value;
    const float since_last = t >= last.time ? t - last.time : t + 1.0f - last.time;
    const float u = std::clamp(since_last / gap, 0.0f, 1.0f);
    return hermite(last.value, last.tangent_out * gap, first.value, first.tangent_in * gap, u);
}

Track* TrackSet::find(std::string_view name) {
    auto it = std::find_if(tracks.begin(), tracks.end(),
                           [&](const Track& t) { return t.name == name; });
    return it != tracks.end() ? &*it : nullptr;
}

const Track* TrackSet::find(std::string_view name) const {
    auto it = std::find_if(tracks.begin(), tracks.end(),
                           [&](const Track& t) { return t.name == name; });
    return it != tracks.end() ? &*it : nullptr;
}

Track& TrackSet::add(std::string name, TrackKind kind, TrackMode mode) {
    Track fresh;
    fresh.name = std::move(name);
    fresh.kind = kind;
    fresh.mode = mode;
    if (Track* existing = find(fresh.name)) {
        *existing = std::move(fresh);
        return *existing;
    }
    tracks.push_back(std::move(fresh));
    return tracks.back();
}

void TrackSet::remove(std::string_view name) {
    std::erase_if(tracks, [&](const Track& t) { return t.name == name; });
}

std::optional<float> TrackSet::value_of(std::string_view name, float x) const {
    const Track* track = find(name);
    if (track == nullptr || track->kind != TrackKind::Float || track->curve.keyframes.empty()) {
        return std::nullopt;
    }
    const float t = normalize_time(x, length, wrap);
    return wrap ? evaluate_periodic(track->curve, t) : track->curve.evaluate(t);
}

std::optional<glm::vec3> TrackSet::color_of(std::string_view name, float x) const {
    const Track* track = find(name);
    if (track == nullptr || track->kind != TrackKind::Color || track->gradient.keys.empty()) {
        return std::nullopt;
    }
    return track->gradient.evaluate(normalize_time(x, length, wrap), wrap);
}

void TrackSet::apply(std::string_view name, float x, float& target) const {
    const auto value = value_of(name, x);
    if (!value) return;
    const Track* track = find(name);
    target = track->mode == TrackMode::Multiply ? target * *value : *value;
}

void TrackSet::apply(std::string_view name, float x, glm::vec3& target) const {
    const auto value = color_of(name, x);
    if (!value) return;
    const Track* track = find(name);
    target = track->mode == TrackMode::Multiply ? target * *value : *value;
}

namespace {

// One component of a named value, folded in the space it is scaled in and
// written back in the space it is held in.
void fold_component(float& held, bool in_linear, TrackMode mode, float value, float weight) {
    float current = in_linear ? color_space::srgb_to_linear(held) : held;
    if (mode == TrackMode::Multiply) {
        current *= 1.0f + (value - 1.0f) * weight;
    } else {
        current += (value - current) * weight;
    }
    held = in_linear ? color_space::linear_to_srgb(current) : current;
}

} // namespace

void fold_track(NamedValue& target, TrackMode mode, float value, float weight) {
    if (target.count < 1 || target.srgb) return;
    fold_component(target.values[0], false, mode, value, weight);
}

void fold_track(NamedValue& target, TrackMode mode, const glm::vec3& value, float weight) {
    if (target.count < 3) return;
    for (uint32_t i = 0; i < 3; ++i) {
        fold_component(target.values[i], target.srgb, mode, value[i], weight);
    }
}

void TrackSet::to_json(nlohmann::json& j) const {
    j = nlohmann::json::object();
    j["length"] = length;
    j["wrap"] = wrap;
    auto& jt = j["tracks"] = nlohmann::json::array();
    for (const auto& track : tracks) {
        nlohmann::json entry;
        entry["name"] = track.name;
        entry["mode"] = mode_name(track.mode);
        if (track.kind == TrackKind::Float) {
            entry["type"] = "float";
            auto& keys = entry["keys"] = nlohmann::json::array();
            for (const auto& k : track.curve.keyframes) {
                // Times and tangents in domain units, the way they are edited.
                keys.push_back({{"t", k.time * length},
                                {"v", k.value},
                                {"in", k.tangent_in / std::max(length, 1e-6f)},
                                {"out", k.tangent_out / std::max(length, 1e-6f)}});
            }
        } else {
            entry["type"] = "color";
            nlohmann::json keys;
            track.gradient.to_json(keys);
            for (auto& k : keys) k["t"] = k["t"].get<float>() * length;
            entry["keys"] = std::move(keys);
        }
        jt.push_back(std::move(entry));
    }
}

void TrackSet::from_json(const nlohmann::json& j) {
    tracks.clear();
    length = j.value("length", 1.0f);
    if (length <= 0.0f) length = 1.0f;
    wrap = j.value("wrap", false);
    if (!j.contains("tracks") || !j["tracks"].is_array()) return;
    for (const auto& entry : j["tracks"]) {
        Track track;
        track.name = entry.value("name", std::string{});
        if (track.name.empty()) continue;
        track.mode = mode_from(entry.value("mode", std::string{"override"}));
        const std::string type = entry.value("type", std::string{"float"});
        if (type == "color") {
            track.kind = TrackKind::Color;
            if (entry.contains("keys")) {
                nlohmann::json keys = entry["keys"];
                for (auto& k : keys) {
                    if (k.contains("t")) k["t"] = k["t"].get<float>() / length;
                }
                track.gradient.from_json(keys);
            }
        } else {
            track.kind = TrackKind::Float;
            if (entry.contains("keys") && entry["keys"].is_array()) {
                for (const auto& k : entry["keys"]) {
                    CurveKeyframe key;
                    key.time = std::clamp(k.value("t", 0.0f) / length, 0.0f, 1.0f);
                    key.value = k.value("v", 0.0f);
                    key.tangent_in = k.value("in", 0.0f) * length;
                    key.tangent_out = k.value("out", 0.0f) * length;
                    track.curve.keyframes.push_back(key);
                }
                std::stable_sort(track.curve.keyframes.begin(), track.curve.keyframes.end(),
                                 [](const CurveKeyframe& a, const CurveKeyframe& b) {
                                     return a.time < b.time;
                                 });
            }
        }
        tracks.push_back(std::move(track));
    }
}

} // namespace fjell
