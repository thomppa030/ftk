#include "scene/component_registry.hpp"
#include "animation/animator_component.hpp"
#include "scene/node.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "animation/animation_clip.hpp"
#include "animation/skeleton.hpp"
#include "core/base_path.hpp"
#include "ftk/base/log.hpp"
#include "core/project_manager.hpp"

#include <nlohmann/json.hpp>

#include <cstring>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<AnimatorComponent>({
        .display_name = "Animator",
        .json_key = "animator",

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<AnimatorComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* /*proj*/) {
            auto* anim = static_cast<const AnimatorComponent*>(ptr);
            if (!anim->clip_paths.empty()) out["clips"] = anim->clip_paths;
            if (!anim->animset_path.empty()) out["animset"] = anim->animset_path;
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* proj) {
            if (node.components.has<AnimatorComponent>()) return;
            auto& anim = node.components.add<AnimatorComponent>();
            if (const auto* smc = node.components.get<SkeletalMeshComponent>()) {
                if (smc->skeletal_model && smc->skeletal_model->skeleton()) anim.init(smc->skeletal_model->skeleton());
            }
            if (j.contains("clips") && j["clips"].is_array()) {
                for (const auto& jc : j["clips"]) {
                    std::string clip_rel = jc.get<std::string>();
                    anim.clip_paths.push_back(clip_rel);
                    auto result = AnimationClip::load(base_path::resolve_asset(clip_rel, proj));
                    if (result) {
                        auto clip = std::make_shared<AnimationClip>(std::move(*result));
                        anim.clips.push_back(clip);
                        if (anim.clips.size() == 1) anim.play(clip.get());
                    } else {
                        FJELL_CORE_ERROR("Failed to load clip {}: {}", clip_rel, result.error());
                    }
                }
            }
            if (j.contains("animset")) {
                anim.animset_path = j["animset"].get<std::string>();
                auto resolved = base_path::resolve_asset(anim.animset_path, proj);
                if (auto r = anim.load_state_machine(resolved); !r) {
                    FJELL_CORE_ERROR("Failed to load animset {}: {}", anim.animset_path, r.error());
                }
                for (size_t i = 0; i < anim.clips.size(); ++i) {
                    if (i < anim.clip_paths.size()) anim.set_clip(anim.clip_paths[i], anim.clips[i].get());
                }
                std::string base_dir = proj ? proj->root().string() : "";
                anim.resolve_blend_spaces(base_dir);
            }
        },

        .script_type_name = "AnimatorComponent",

        // Replicate the state-machine parameters that gameplay code feeds the
        // animator (e.g. "speed", "is_jumping"). Remote instances run the same
        // FSM locally off these, so transitions and blends are reproduced without
        // streaming raw clip/time. Single-clip playback has no params and so
        // contributes nothing here.
        .net_serialize = [](const void* ptr, std::vector<uint8_t>& out) {
            const auto* anim = static_cast<const AnimatorComponent*>(ptr);
            const auto& params =
                const_cast<AnimatorComponent*>(anim)->state_machine().params_mutable();
            auto put_u16 = [&](uint16_t v) {
                out.push_back(static_cast<uint8_t>(v & 0xFF));
                out.push_back(static_cast<uint8_t>(v >> 8));
            };
            put_u16(static_cast<uint16_t>(params.size()));
            for (const auto& [name, value] : params) {
                put_u16(static_cast<uint16_t>(name.size()));
                out.insert(out.end(), name.begin(), name.end());
                const auto* vp = reinterpret_cast<const uint8_t*>(&value);
                out.insert(out.end(), vp, vp + sizeof(float));
            }
        },

        .net_deserialize = [](void* ptr, std::span<const uint8_t> in) {
            auto* anim = static_cast<AnimatorComponent*>(ptr);
            size_t pos = 0;
            auto take_u16 = [&](uint16_t& v) -> bool {
                if (pos + 2 > in.size()) return false;
                v = static_cast<uint16_t>(in[pos] | (in[pos + 1] << 8));
                pos += 2;
                return true;
            };
            uint16_t count = 0;
            if (!take_u16(count)) return;
            for (uint16_t i = 0; i < count; ++i) {
                uint16_t name_len = 0;
                if (!take_u16(name_len)) return;
                if (pos + name_len + sizeof(float) > in.size()) return;
                std::string name(reinterpret_cast<const char*>(in.data() + pos), name_len);
                pos += name_len;
                float value = 0.0f;
                std::memcpy(&value, in.data() + pos, sizeof(float));
                pos += sizeof(float);
                anim->set_param(name, value);
            }
        },
    });
    return true;
}();

} // namespace fjell
