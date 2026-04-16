#include "scripting/asset_type_registry.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "core/base_path.hpp"
#include "core/project.hpp"
#include "ui/icon_cache.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <fstream>

namespace {

using fjell::AssetTypeRegistry;
using fjell::ResourceManager;

static const bool reg_vfx = [] {
    AssetTypeRegistry::instance().register_type({
        .type_name = "VfxAsset",
        .extension = ".fjvfx",
        .label     = "VFX Effect",
        .resolve   = [](ResourceManager& rm, uint32_t id) -> void* {
            return rm.get(fjell::VfxAssetHandle{id});
        },
        .path_of   = [](const ResourceManager& rm, uint32_t id) -> std::string {
            return rm.asset_path_of(fjell::VfxAssetHandle{id});
        },
        .load      = [](ResourceManager& rm, const std::string& path) -> fjell::Result<uint32_t> {
            auto r = rm.load_vfx_asset(path);
            if (!r) return fjell::make_error(r.error());
            return r->id;
        },
    });
    return true;
}();

static const bool reg_audio = [] {
    AssetTypeRegistry::instance().register_type({
        .type_name = "AudioAsset",
        .extension = ".fjaudio",
        .label     = "Audio Clip",
        .resolve   = [](ResourceManager& rm, uint32_t id) -> void* {
            return rm.get(fjell::AudioAssetHandle{id});
        },
        .path_of   = [](const ResourceManager& rm, uint32_t id) -> std::string {
            return rm.asset_path_of(fjell::AudioAssetHandle{id});
        },
        .load      = [](ResourceManager& rm, const std::string& path) -> fjell::Result<uint32_t> {
            auto r = rm.load_audio_asset(path);
            if (!r) return fjell::make_error(r.error());
            return r->id;
        },
    });
    return true;
}();

static const bool reg_material = [] {
    AssetTypeRegistry::instance().register_type({
        .type_name = "Material",
        .extension = ".fjmat",
        .label     = "Material",
        .resolve   = [](ResourceManager& rm, uint32_t id) -> void* {
            return rm.get(fjell::MaterialHandle{id});
        },
        .path_of   = [](const ResourceManager& rm, uint32_t id) -> std::string {
            return rm.asset_path_of(fjell::MaterialHandle{id});
        },
        .load      = [](ResourceManager& rm, const std::string& path) -> fjell::Result<uint32_t> {
            auto r = rm.load_material_asset(path);
            if (!r) return fjell::make_error(r.error());
            return r->id;
        },
        .draw_preview = [](const std::string& path, ResourceManager& /*rm*/,
                           fjell::IconCache* icons, const fjell::Project* project,
                           float width) {
            if (!icons || !project || path.empty() || path == "default") return;

            // Resolve the .fjmat to an absolute path and parse it for base_color texture
            auto abs_mat = fjell::base_path::resolve_asset(path, project);
            std::ifstream file(abs_mat);
            if (!file.is_open()) return;

            nlohmann::json j;
            try { j = nlohmann::json::parse(file); } catch (...) { return; }

            if (!j.contains("textures") || !j["textures"].contains("base_color")) return;
            auto tex_rel = j["textures"]["base_color"].get<std::string>();
            if (tex_rel.empty()) return;

            auto abs_tex = (project->root() / tex_rel).string();
            auto thumb = icons->thumbnail(abs_tex);
            if (thumb) {
                ImGui::Image(thumb, {width, width});
            }
        },
    });
    return true;
}();

static const bool reg_input_action = [] {
    AssetTypeRegistry::instance().register_type({
        .type_name = "InputAction",
        .extension = ".fjaction",
        .label     = "Input Action",
        .resolve   = [](ResourceManager& rm, uint32_t id) -> void* {
            return rm.get(fjell::InputActionHandle{id});
        },
        .path_of   = [](const ResourceManager& rm, uint32_t id) -> std::string {
            return rm.asset_path_of(fjell::InputActionHandle{id});
        },
        .load      = [](ResourceManager& rm, const std::string& path) -> fjell::Result<uint32_t> {
            auto r = rm.load_input_action(path);
            if (!r) return fjell::make_error(r.error());
            return r->id;
        },
    });
    return true;
}();

} // anonymous namespace
