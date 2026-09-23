#include "scene/component_registry.hpp"
#include "core/base_path.hpp"
#include "audio/audio_component.hpp"
#include "scene/node.hpp"

#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<AudioSourceComponent>({
        .display_name = "Audio Source",
        .json_key = "audio",

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<AudioSourceComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* proj) {
            auto* au = static_cast<const AudioSourceComponent*>(ptr);
            out["clip"] = base_path::asset_reference(au->clip_path, proj);
            out["volume"] = au->volume;
            out["pitch"] = au->pitch;
            out["min_distance"] = au->min_distance;
            out["max_distance"] = au->max_distance;
            out["loop"] = au->loop;
            out["spatial"] = au->spatial;
            out["play_on_start"] = au->play_on_start;
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            auto& au = node.components.add<AudioSourceComponent>();
            au.clip_path = j.value("clip", "");
            au.volume = j.value("volume", 1.0f);
            au.pitch = j.value("pitch", 1.0f);
            au.min_distance = j.value("min_distance", 1.0f);
            au.max_distance = j.value("max_distance", 50.0f);
            au.loop = j.value("loop", false);
            au.spatial = j.value("spatial", true);
            au.play_on_start = j.value("play_on_start", false);
        },

        .script_type_name = {},
    });
    return true;
}();

} // namespace fjell
