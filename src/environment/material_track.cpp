#include "environment/material_track.hpp"

#include "environment/environment_state.hpp"

namespace fjell {

namespace material_track {

namespace {

struct Prefix {
    std::string_view text;
    Kind kind;
    std::string_view heading;
};
// The one place a kind's spelling is written down.
constexpr Prefix PREFIXES[] = {
    {"fog.", Kind::Fog, "Fog"},
    {"post.", Kind::Post, "Post"},
};

const Prefix& prefix_of(Kind kind) {
    for (const auto& p : PREFIXES) {
        if (p.kind == kind) return p;
    }
    return PREFIXES[0];
}

// The names from before the fog had a material.
constexpr std::string_view LEGACY_DENSITY = "fog_density";
constexpr std::string_view LEGACY_COLOR = "fog_color";

std::optional<Target> parse_legacy(std::string_view name, std::string_view legacy,
                                   std::string_view uniform) {
    if (name == legacy) return Target{Kind::Fog, {}, std::string(uniform)};
    if (name.size() > legacy.size() + 1 && name.starts_with(legacy) && name[legacy.size()] == '.') {
        return Target{Kind::Fog, std::string(name.substr(legacy.size() + 1)), std::string(uniform)};
    }
    return std::nullopt;
}

} // namespace

std::optional<Target> parse(std::string_view track_name) {
    if (auto legacy = parse_legacy(track_name, LEGACY_DENSITY, FOG_DENSITY_UNIFORM)) return legacy;
    if (auto legacy = parse_legacy(track_name, LEGACY_COLOR, FOG_COLOR_UNIFORM)) return legacy;
    for (const auto& prefix : PREFIXES) {
        if (!track_name.starts_with(prefix.text) || track_name.size() <= prefix.text.size()) continue;
        // `<prefix>.<uniform>` or `<prefix>.<group>.<uniform>`: a uniform is a
        // GLSL identifier and has no dot in it, so the last dot is the split.
        const std::string_view rest = track_name.substr(prefix.text.size());
        const size_t last_dot = rest.rfind('.');
        if (last_dot == std::string_view::npos) return Target{prefix.kind, {}, std::string(rest)};
        if (last_dot == 0 || last_dot + 1 >= rest.size()) return std::nullopt;
        return Target{prefix.kind, std::string(rest.substr(0, last_dot)),
                      std::string(rest.substr(last_dot + 1))};
    }
    return std::nullopt;
}

std::string name_for(Kind kind, std::string_view group, std::string_view uniform) {
    std::string name(prefix_of(kind).text);
    if (!group.empty()) {
        name += group;
        name += '.';
    }
    name += uniform;
    return name;
}

std::string heading(const Target& target) {
    std::string text(prefix_of(target.kind).heading);
    if (!target.group.empty()) text += " \xc2\xb7 " + target.group;
    return text;
}

namespace {

template <typename Value>
void fold_into(std::vector<NamedValue>& uniforms, const Target& target, TrackMode mode,
               const Value& value, float weight) {
    if (auto* u = find_named(uniforms, target.uniform)) fold_track(*u, mode, value, weight);
}

template <typename Value>
void apply_to(EnvironmentState& state, const Target& target, TrackMode mode,
              const Value& value, float weight) {
    switch (target.kind) {
        case Kind::Fog:
            if (target.group.empty()) {
                fold_into(state.fog.uniforms, target, mode, value, weight);
                return;
            }
            for (auto& volume : state.fog_volumes) {
                if (volume.group == target.group) {
                    fold_into(volume.uniforms, target, mode, value, weight);
                }
            }
            return;
        case Kind::Post:
            // Without a group, the scene's base look: every volume that is
            // everywhere. A bounded one only answers to its group.
            for (auto& volume : state.post_volumes) {
                const bool reached = target.group.empty() ? volume.infinite
                                                          : volume.group == target.group;
                if (reached) fold_into(volume.uniforms, target, mode, value, weight);
            }
            return;
    }
}

} // namespace

void apply(EnvironmentState& state, const Target& target, TrackMode mode, float value,
           float weight) {
    apply_to(state, target, mode, value, weight);
}

void apply(EnvironmentState& state, const Target& target, TrackMode mode,
           const glm::vec3& value, float weight) {
    apply_to(state, target, mode, value, weight);
}

} // namespace material_track

} // namespace fjell
