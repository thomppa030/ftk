#include "animation/animation_clip.hpp"
#include "core/asset_identity.hpp"
#include "animation/anim_notify_registry.hpp"
#include "scripting/field_serialization.hpp"
#include "core/log.hpp"

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>

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

namespace {

// Notifies from their JSON, as the clip file holds them; a type that isn't
// loaded keeps its properties so saving writes them back unchanged.
std::vector<AnimNotifyInstance> notifies_from_json(const nlohmann::json& notifies, const std::string& where) {
    std::vector<AnimNotifyInstance> out;
    auto& registry = AnimNotifyRegistry::instance();
    for (const auto& jn : notifies) {
        AnimNotifyInstance ni;
        ni.time = jn.value("time", 0.0f);
        ni.end_time = jn.value("end_time", 0.0f);
        ni.type_name = jn.value("type", "");
        ni.properties = jn; // keep full JSON for re-serialization

        if (!ni.type_name.empty()) {
            ni.instance = registry.create(ni.type_name);
            if (ni.instance) {
                const auto* info = registry.find(ni.type_name);
                if (info && !info->fields.empty()) {
                    deserialize_reflected_fields(info->fields, ni.instance.get(), jn);
                }
            } else {
                FJELL_CORE_WARN("Animation {}: unknown notify type '{}' at t={:.2f}",
                                where, ni.type_name, ni.time);
            }
        }
        out.push_back(std::move(ni));
    }
    return out;
}

} // namespace

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

    // Parse notifies (typed AnimNotify instances)
    if (j.contains("notifies") && j["notifies"].is_array()) {
        clip.notifies_ = notifies_from_json(j["notifies"], path);
    }

    // Backward compat: load old "events" as untyped notifies (properties only)
    if (j.contains("events") && j["events"].is_array()) {
        for (const auto& je : j["events"]) {
            AnimNotifyInstance ni;
            ni.time = je.value("time", 0.0f);
            ni.type_name = je.value("name", "event");
            ni.properties = je;
            clip.notifies_.push_back(std::move(ni));
        }
    }

    // Warn on unknown top-level fields
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() != "name" && it.key() != "tracks" &&
            it.key() != "notifies" && it.key() != "events" &&
            it.key() != asset_identity::UUID_KEY) {
            FJELL_CORE_WARN("Animation {}: unknown field '{}'", path, it.key());
        }
    }

    // Build ozz compressed animation
    auto anim_result = build_ozz_animation(clip.name_, clip.tracks_);
    if (!anim_result) {
        return make_error(anim_result.error());
    }
    clip.animation_ = std::move(*anim_result);

    FJELL_CORE_DEBUG("Loaded animation clip: {} ({:.2f}s, {} tracks)",
                    clip.name_, clip.animation_.duration(), clip.animation_.num_tracks());
    return clip;
}

namespace {

// The notifies as the clip file holds them: the reflected fields of each
// live instance, or the stored properties of a type that isn't loaded.
nlohmann::json notifies_json(const std::vector<AnimNotifyInstance>& notifies) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& ni : notifies) {
        nlohmann::json jn;
        jn["time"] = ni.time;
        if (ni.end_time > 0.0f) jn["end_time"] = ni.end_time;
        jn["type"] = ni.type_name;

        // Serialize reflected fields from live instance, or fall back to stored properties
        if (ni.instance) {
            const auto* info = AnimNotifyRegistry::instance().find(ni.type_name);
            if (info && !info->fields.empty()) {
                serialize_reflected_fields(info->fields, ni.instance.get(), jn);
            }
        } else if (!ni.properties.empty()) {
            // Type not loaded — preserve original properties
            for (auto it = ni.properties.begin(); it != ni.properties.end(); ++it) {
                if (it.key() != "time" && it.key() != "end_time" && it.key() != "type") {
                    jn[it.key()] = it.value();
                }
            }
        }
        out.push_back(jn);
    }

    return out;
}

} // namespace

std::string AnimationClip::notifies_text() const {
    return notifies_json(notifies_).dump(2);
}

Result<> AnimationClip::set_notifies_from_text(std::string_view text) {
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_array()) return make_error("notifies aren't a JSON list");
    notifies_ = notifies_from_json(j, name_);
    return {};
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

    j["notifies"] = notifies_json(notifies_);

    std::ofstream file(path);
    if (!file.is_open()) {
        return make_error("Failed to save animation: " + path);
    }
    file << j.dump(2);
    FJELL_CORE_INFO("Saved animation clip: {}", path);
    return {};
}

} // namespace fjell
