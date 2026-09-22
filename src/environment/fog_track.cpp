#include "environment/fog_track.hpp"

#include "environment/environment_state.hpp"

namespace fjell {

namespace fog_track {

namespace {

// The names from before the fog had a material.
constexpr std::string_view LEGACY_DENSITY = "fog_density";
constexpr std::string_view LEGACY_COLOR = "fog_color";

std::optional<Target> parse_legacy(std::string_view name, std::string_view legacy,
                                   std::string_view uniform) {
    if (name == legacy) return Target{{}, std::string(uniform)};
    if (name.size() > legacy.size() + 1 && name.starts_with(legacy) && name[legacy.size()] == '.') {
        return Target{std::string(name.substr(legacy.size() + 1)), std::string(uniform)};
    }
    return std::nullopt;
}

} // namespace

std::optional<Target> parse(std::string_view track_name) {
    if (auto legacy = parse_legacy(track_name, LEGACY_DENSITY, DENSITY_UNIFORM)) return legacy;
    if (auto legacy = parse_legacy(track_name, LEGACY_COLOR, COLOR_UNIFORM)) return legacy;
    if (!track_name.starts_with(PREFIX) || track_name.size() <= PREFIX.size()) return std::nullopt;

    // `fog.<uniform>` or `fog.<group>.<uniform>`: a uniform is a GLSL
    // identifier and has no dot in it, so the last dot is the split.
    const std::string_view rest = track_name.substr(PREFIX.size());
    const size_t last_dot = rest.rfind('.');
    if (last_dot == std::string_view::npos) return Target{{}, std::string(rest)};
    if (last_dot == 0 || last_dot + 1 >= rest.size()) return std::nullopt;
    return Target{std::string(rest.substr(0, last_dot)), std::string(rest.substr(last_dot + 1))};
}

std::string name_for(std::string_view group, std::string_view uniform) {
    std::string name(PREFIX);
    if (!group.empty()) {
        name += group;
        name += '.';
    }
    name += uniform;
    return name;
}

namespace {

template <typename Value>
void apply_to(EnvironmentState& state, const Target& target, TrackMode mode,
              const Value& value, float weight) {
    if (target.group.empty()) {
        if (auto* u = find_named(state.fog.uniforms, target.uniform)) {
            fold_track(*u, mode, value, weight);
        }
        return;
    }
    for (auto& volume : state.fog_volumes) {
        if (volume.group != target.group) continue;
        if (auto* u = find_named(volume.uniforms, target.uniform)) {
            fold_track(*u, mode, value, weight);
        }
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

} // namespace fog_track

} // namespace fjell
