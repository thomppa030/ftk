#include "core/asset_type_registry.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/preset_asset.hpp"
#include "core/base_path.hpp"
#include "core/project.hpp"
#include "ui/icon_cache.hpp"
#include "ui/asset_thumbnail_renderer.hpp"

#include <imgui.h>

#include <algorithm>

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

static const bool reg_surface = [] {
    AssetTypeRegistry::instance().register_type({
        .type_name = "SurfaceAsset",
        .extension = ".fjsurface",
        .label     = "Surface",
        .resolve   = [](ResourceManager& rm, uint32_t id) -> void* {
            return rm.get(fjell::SurfaceAssetHandle{id});
        },
        .path_of   = [](const ResourceManager& rm, uint32_t id) -> std::string {
            return rm.asset_path_of(fjell::SurfaceAssetHandle{id});
        },
        .load      = [](ResourceManager& rm, const std::string& path) -> fjell::Result<uint32_t> {
            auto r = rm.load_surface_asset(path);
            if (!r) return fjell::make_error(r.error());
            return r->id;
        },
    });
    return true;
}();

static const bool reg_day_profile = [] {
    AssetTypeRegistry::instance().register_type({
        .type_name = "DayProfileAsset",
        .extension = ".fjday",
        .label     = "Day Profile",
        .resolve   = [](ResourceManager& rm, uint32_t id) -> void* {
            return rm.get(fjell::DayProfileAssetHandle{id});
        },
        .path_of   = [](const ResourceManager& rm, uint32_t id) -> std::string {
            return rm.asset_path_of(fjell::DayProfileAssetHandle{id});
        },
        .load      = [](ResourceManager& rm, const std::string& path) -> fjell::Result<uint32_t> {
            auto r = rm.load_day_profile(path);
            if (!r) return fjell::make_error(r.error());
            return r->id;
        },
    });
    return true;
}();

static const bool reg_weather = [] {
    AssetTypeRegistry::instance().register_type({
        .type_name = "WeatherAsset",
        .extension = ".fjweather",
        .label     = "Weather",
        .resolve   = [](ResourceManager& rm, uint32_t id) -> void* {
            return rm.get(fjell::WeatherAssetHandle{id});
        },
        .path_of   = [](const ResourceManager& rm, uint32_t id) -> std::string {
            return rm.asset_path_of(fjell::WeatherAssetHandle{id});
        },
        .load      = [](ResourceManager& rm, const std::string& path) -> fjell::Result<uint32_t> {
            auto r = rm.load_weather_asset(path);
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
            auto* thumbnails = fjell::AssetThumbnailRenderer::instance();
            if (!thumbnails || !project || path.empty() || path == "default") return;

            auto abs_mat = fjell::base_path::resolve_asset(path, project);
            if (abs_mat.empty()) return;

            // The sphere render streams in a frame or two after the first
            // request; the material icon holds the space until then.
            auto thumb = thumbnails->descriptor(abs_mat);
            if (!thumb) {
                thumbnails->request(abs_mat);
                thumb = icons ? icons->icon("material") : VK_NULL_HANDLE;
            }
            // Drawn at the thumbnail's own size: stretched to the panel it
            // is a blur, and a sphere does not need to be wider than that.
            float size = std::min(width, static_cast<float>(fjell::AssetThumbnailRenderer::SIZE));
            if (thumb) {
                ImGui::Image(thumb, {size, size});
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

static const bool reg_preset = [] {
    AssetTypeRegistry::instance().register_type({
        .type_name = "PresetAsset",
        .extension = ".fjp",
        .label     = "Preset",
        .resolve   = [](ResourceManager& rm, uint32_t id) -> void* {
            return rm.get(fjell::PresetAssetHandle{id});
        },
        .path_of   = [](const ResourceManager& rm, uint32_t id) -> std::string {
            return rm.asset_path_of(fjell::PresetAssetHandle{id});
        },
        .load      = [](ResourceManager& rm, const std::string& path) -> fjell::Result<uint32_t> {
            auto r = rm.load_preset_asset(path);
            if (!r) return fjell::make_error(r.error());
            return r->id;
        },
    });
    return true;
}();

} // anonymous namespace
