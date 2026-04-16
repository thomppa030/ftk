#include "scene/component_registry.hpp"
#include "audio/audio_component.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<AudioSourceComponent>({
        .display_name = "Audio Source",
        .json_key = "audio",
        .color = {0.55f, 0.70f, 0.85f, 1.0f},

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& ctx) -> bool {
            auto* au = static_cast<AudioSourceComponent*>(ptr);
            bool changed = false;
            static const std::string audio_exts[] = {".wav", ".ogg", ".mp3", ".fjaudio"};
            if (asset_ref_field({.label = "Clip", .id = "##AudioClip",
                                 .extensions = audio_exts, .ctx = &ctx}, au->clip_path)) {
                au->sound_loaded = false; changed = true;
            }
            ImGui::Text("Volume"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##AudioVolume", &au->volume, 0.01f, 0.0f, 2.0f)) changed = true;
            ImGui::Text("Pitch"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##AudioPitch", &au->pitch, 0.01f, 0.1f, 4.0f)) changed = true;
            if (ImGui::Checkbox("Loop", &au->loop)) changed = true;
            if (ImGui::Checkbox("Spatial", &au->spatial)) changed = true;
            if (ImGui::Checkbox("Play on Start", &au->play_on_start)) changed = true;
            if (au->spatial) {
                ImGui::Text("Min Distance"); ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat("##AudioMinDist", &au->min_distance, 0.1f, 0.1f, 100.0f)) changed = true;
                ImGui::Text("Max Distance"); ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat("##AudioMaxDist", &au->max_distance, 1.0f, 1.0f, 1000.0f)) changed = true;
            }
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<AudioSourceComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out) {
            auto* au = static_cast<const AudioSourceComponent*>(ptr);
            out["clip"] = au->clip_path;
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
