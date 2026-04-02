#include "renderer/systems/vfx_asset.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/systems/vfx_module.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace fjell {

// ── VfxEffect v2 load/save ────────────────────────────────────────────

static VfxEmitter load_emitter_v2(const nlohmann::json& je) {
    VfxEmitter emitter;
    emitter.name = je.value("name", "emitter");
    emitter.max_particles = je.value("max_particles", 10000u);
    emitter.blend_mode = static_cast<ParticleBlendMode>(je.value("blend_mode", 0));

    auto load_modules = [](const nlohmann::json& arr, std::vector<std::unique_ptr<VfxModule>>& out) {
        for (const auto& jm : arr) {
            auto type_name = jm.value("type", "");
            auto mod = create_module(type_name);
            if (mod) {
                mod->from_json(jm);
                out.push_back(std::move(mod));
            }
        }
    };

    if (je.contains("spawn")) load_modules(je["spawn"], emitter.spawn_modules);
    if (je.contains("update")) load_modules(je["update"], emitter.update_modules);
    if (je.contains("render")) load_modules(je["render"], emitter.render_modules);

    return emitter;
}

static nlohmann::json save_emitter_v2(const VfxEmitter& emitter) {
    nlohmann::json je;
    je["name"] = emitter.name;
    je["max_particles"] = emitter.max_particles;
    je["blend_mode"] = static_cast<int>(emitter.blend_mode);

    auto save_modules = [](const std::vector<std::unique_ptr<VfxModule>>& mods) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& m : mods) {
            if (!m->enabled) continue;
            nlohmann::json jm;
            m->to_json(jm);
            arr.push_back(jm);
        }
        return arr;
    };

    je["spawn"] = save_modules(emitter.spawn_modules);
    je["update"] = save_modules(emitter.update_modules);
    je["render"] = save_modules(emitter.render_modules);

    return je;
}

VfxEffect load_vfx_effect(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open .fjvfx file: " + path);
    }

    nlohmann::json j;
    f >> j;

    VfxEffect effect;
    effect.name = j.value("name", "effect");
    if (j.contains("emitters")) {
        for (const auto& je : j["emitters"]) {
            effect.emitters.push_back(load_emitter_v2(je));
        }
    }
    return effect;
}

void save_vfx_effect(const std::string& path, const VfxEffect& effect) {
    nlohmann::json j;
    j["version"] = 2;
    j["name"] = effect.name;

    nlohmann::json emitters_arr = nlohmann::json::array();
    for (const auto& emitter : effect.emitters) {
        emitters_arr.push_back(save_emitter_v2(emitter));
    }
    j["emitters"] = emitters_arr;

    std::ofstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to write .fjvfx file: " + path);
    }
    f << j.dump(4) << "\n";
}

} // namespace fjell
