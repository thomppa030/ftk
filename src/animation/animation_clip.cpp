#include "animation/animation_clip.hpp"
#include "core/log.hpp"

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace fjell {

namespace {

// Build an ozz compressed Animation from raw track data
Result<ozz::animation::Animation> build_ozz_animation(
        const std::string& name,
        const std::vector<AnimJointTrack>& tracks) {
    ozz::animation::offline::RawAnimation raw;
    raw.name = name.c_str();

    // Compute duration from latest keyframe across all tracks
    float max_time = 0.0f;
    for (const auto& track : tracks) {
        for (const auto& k : track.translations) {
            max_time = std::max(max_time, k.time);
        }
        for (const auto& k : track.rotations) {
            max_time = std::max(max_time, k.time);
        }
        for (const auto& k : track.scales) {
            max_time = std::max(max_time, k.time);
        }
    }
    raw.duration = max_time > 0.0f ? max_time : 1.0f;

    raw.tracks.resize(tracks.size());
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
        auto& out = raw.tracks[i];
        const auto& in = tracks[i];

        for (const auto& k : in.translations) {
            out.translations.push_back({k.time,
                ozz::math::Float3(k.value.x, k.value.y, k.value.z)});
        }
        for (const auto& k : in.rotations) {
            out.rotations.push_back({k.time,
                ozz::math::Quaternion(k.value.x, k.value.y, k.value.z, k.value.w)});
        }
        for (const auto& k : in.scales) {
            out.scales.push_back({k.time,
                ozz::math::Float3(k.value.x, k.value.y, k.value.z)});
        }
    }

    if (!raw.Validate()) {
        return make_error("Invalid animation data for clip: " + name);
    }

    ozz::animation::offline::AnimationBuilder builder;
    auto result = builder(raw);
    if (!result) {
        return make_error("Failed to build ozz animation for clip: " + name);
    }

    return std::move(*result);
}

} // anonymous namespace

Result<AnimationClip> AnimationClip::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return make_error("Failed to open animation: " + path);
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        return make_error("Failed to parse animation JSON: " + path + " — " + e.what());
    }

    AnimationClip clip;
    clip.name_ = j.value("name", "");

    // Parse tracks
    if (!j.contains("tracks") || !j["tracks"].is_array()) {
        return make_error("Animation missing 'tracks' array: " + path);
    }

    const auto& jtracks = j["tracks"];
    clip.tracks_.reserve(jtracks.size());

    for (const auto& jt : jtracks) {
        AnimJointTrack track;

        if (jt.contains("translations")) {
            for (const auto& jk : jt["translations"]) {
                AnimKeyVec3 key;
                key.time = jk[0].get<float>();
                const auto& v = jk[1];
                key.value = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
                track.translations.push_back(key);
            }
        }
        if (jt.contains("rotations")) {
            for (const auto& jk : jt["rotations"]) {
                AnimKeyQuat key;
                key.time = jk[0].get<float>();
                const auto& v = jk[1];
                // JSON stores [x, y, z, w], glm::quat constructor is (w, x, y, z)
                key.value = glm::quat(v[3].get<float>(), v[0].get<float>(),
                                      v[1].get<float>(), v[2].get<float>());
                track.rotations.push_back(key);
            }
        }
        if (jt.contains("scales")) {
            for (const auto& jk : jt["scales"]) {
                AnimKeyVec3 key;
                key.time = jk[0].get<float>();
                const auto& v = jk[1];
                key.value = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
                track.scales.push_back(key);
            }
        }

        clip.tracks_.push_back(std::move(track));
    }

    // Parse events
    if (j.contains("events") && j["events"].is_array()) {
        for (const auto& je : j["events"]) {
            AnimationEvent event;
            event.time = je.value("time", 0.0f);
            event.name = je.value("name", "");
            if (je.contains("params") && je["params"].is_object()) {
                for (auto pit = je["params"].begin(); pit != je["params"].end(); ++pit) {
                    event.params[pit.key()] = pit.value().get<std::string>();
                }
            }
            clip.events_.push_back(std::move(event));
        }
    }

    // Warn on unknown top-level fields
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() != "name" && it.key() != "tracks" && it.key() != "events") {
            FJELL_CORE_WARN("Animation {}: unknown field '{}'", path, it.key());
        }
    }

    // Build ozz compressed animation
    auto anim_result = build_ozz_animation(clip.name_, clip.tracks_);
    if (!anim_result) {
        return make_error(anim_result.error());
    }
    clip.animation_ = std::move(*anim_result);

    FJELL_CORE_INFO("Loaded animation clip: {} ({:.2f}s, {} tracks)",
                    clip.name_, clip.animation_.duration(), clip.animation_.num_tracks());
    return clip;
}

Result<> AnimationClip::save(const std::string& path) const {
    nlohmann::json j;
    j["name"] = name_;
    j["tracks"] = nlohmann::json::array();

    for (const auto& track : tracks_) {
        nlohmann::json jt;

        jt["translations"] = nlohmann::json::array();
        for (const auto& k : track.translations) {
            jt["translations"].push_back({k.time, {k.value.x, k.value.y, k.value.z}});
        }

        jt["rotations"] = nlohmann::json::array();
        for (const auto& k : track.rotations) {
            // JSON stores [x, y, z, w]
            jt["rotations"].push_back({k.time, {k.value.x, k.value.y, k.value.z, k.value.w}});
        }

        jt["scales"] = nlohmann::json::array();
        for (const auto& k : track.scales) {
            jt["scales"].push_back({k.time, {k.value.x, k.value.y, k.value.z}});
        }

        j["tracks"].push_back(jt);
    }

    j["events"] = nlohmann::json::array();
    for (const auto& event : events_) {
        nlohmann::json je;
        je["time"] = event.time;
        je["name"] = event.name;
        if (!event.params.empty()) {
            je["params"] = event.params;
        }
        j["events"].push_back(je);
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        return make_error("Failed to save animation: " + path);
    }
    file << j.dump(2);
    FJELL_CORE_INFO("Saved animation clip: {}", path);
    return {};
}

} // namespace fjell
