#include "fjell/api.h"
#include "fjell/surface_api.hpp"
#include "physics/surface_asset.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scripting/script_globals.hpp"

extern "C" FJELL_SDK_EXPORT const char* fjell_surface_name(uint32_t surface_id) {
    auto* resources = fjell::script_services().resources;
    if (resources == nullptr || surface_id == 0) return "";
    const auto* asset = resources->get(fjell::SurfaceAssetHandle{surface_id});
    return asset != nullptr ? asset->name.c_str() : "";
}
