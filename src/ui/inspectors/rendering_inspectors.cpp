#include "ui/inspectors/rendering_inspectors.hpp"

#include "core/asset_type_registry.hpp"
#include "core/base_path.hpp"
#include "core/project_manager.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/color_field.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ui/icons.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/components/sprite_desc.hpp"
#include "renderer/components/tilemap_component.hpp"
#include "renderer/resources/asset_shader_type.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/component_meta.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/inspectors/asset_rows.hpp"

#include <imgui.h>

#include <string>

namespace fjell {

namespace {

// Lighting for a 2D surface: whether it is lit, and the surface it is lit as.
template <typename Surface>
ftk::ui::Edit lighting(Surface& s, bool& normal_loaded, const InspectorContext& ctx) {
    static const std::string texture_exts[] = {".png", ".jpg", ".jpeg"};
    ftk::ui::Edit edit;
    ftk::ui::subheading("Lighting");
    if (auto t = ftk::ui::PropertyTable("##lighting")) {
        ftk::ui::row("Lit", "Shaded by the scene's lights and global illumination, instead of drawn at full colour.",
                [&] { edit |= ftk::ui::checkbox("##lit", s.lit); });
        if (s.lit) {
            ftk::ui::row("Normal map", [&] {
                if (asset_slot({.label = "Normal map", .id = "##normal", .extensions = texture_exts, .ctx = &ctx},
                               s.normal_path)) {
                    normal_loaded = false;
                    edit = {true, true};
                }
            });
            ftk::ui::row("Roughness", [&] { edit |= ftk::ui::slider("##roughness", s.roughness, 0.0f, 1.0f); });
            ftk::ui::row("Metallic", [&] { edit |= ftk::ui::slider("##metallic", s.metallic, 0.0f, 1.0f); });
            ftk::ui::row("Emission", [&] {
                edit |= ftk::ui::drag("##emission", s.emission, {.speed = 0.05f, .min = 0.0f, .max = 100.0f});
            });
        }
    }
    return edit;
}

} // namespace

ftk::ui::Edit draw_mesh(Node3D& node, void* component, const InspectorContext& ctx) {
    auto& mesh = *static_cast<MeshComponent*>(component);
    static const std::string model_exts[] = {".fjmesh"};
    ftk::ui::Edit edit;
    if (auto t = ftk::ui::PropertyTable("##mesh")) {
        ftk::ui::row("Model", [&] {
            // The model is the node's source: the scene loads it from there.
            std::string model = node.source;
            if (asset_slot({.label = "Model", .id = "##model", .extensions = model_exts, .ctx = &ctx}, model)) {
                if (!model.empty() && ctx.resource_manager != nullptr) {
                    if (auto result = ctx.resource_manager->load_model(
                            base_path::resolve_asset(model, project_of(ctx)))) {
                        mesh.model_handle = *result;
                        mesh.model = ctx.resource_manager->get(mesh.model_handle);
                    }
                }
                node.source = model;
                edit = {true, true};
            }
        });
        if (material_row("##material", mesh.material_name, ctx)) {
            load_material(mesh.material_name, mesh.material_handle, mesh.material, ctx);
            edit = {true, true};
        }
    }
    return edit;
}

ftk::ui::Edit draw_skeletal_mesh(Node3D& node, void* component, const InspectorContext& ctx) {
    auto& skel = *static_cast<SkeletalMeshComponent*>(component);
    static const std::string model_exts[] = {".fjmesh", ".fjskel"};
    ftk::ui::Edit edit;
    if (auto t = ftk::ui::PropertyTable("##skeletal_mesh")) {
        ftk::ui::row("Model", [&] {
            std::string model = node.source;
            if (asset_slot({.label = "Model", .id = "##model", .extensions = model_exts, .ctx = &ctx}, model)) {
                if (!model.empty() && ctx.resource_manager != nullptr) {
                    if (auto result = ctx.resource_manager->load_skeletal_model(
                            base_path::resolve_asset(model, project_of(ctx)))) {
                        skel.skeletal_model_handle = *result;
                        skel.skeletal_model = ctx.resource_manager->get(skel.skeletal_model_handle);
                    }
                }
                node.source = model;
                edit = {true, true};
            }
        });
        if (material_row("##material", skel.material_name, ctx)) {
            load_material(skel.material_name, skel.material_handle, skel.material, ctx);
            edit = {true, true};
        }
    }
    return edit;
}

ftk::ui::Edit draw_sprite(Node3D& /*node*/, void* component, const InspectorContext& ctx) {
    auto& sp = *static_cast<SpriteComponent*>(component);
    static const std::string texture_exts[] = {".png", ".jpg", ".jpeg"};
    ftk::ui::Edit edit;
    if (auto t = ftk::ui::PropertyTable("##sprite")) {
        ftk::ui::row("Texture", [&] {
            if (asset_slot({.label = "Texture", .id = "##texture", .extensions = texture_exts, .ctx = &ctx},
                           sp.texture_path)) {
                sp.texture_loaded = false;
                edit = {true, true};
            }
        });
        // The shader multiplies the tint as linear light.
        ftk::ui::row("Tint", [&] { edit |= ftk::ui::color("##tint", sp.tint, ftk::ui::ColorSpace::Linear); });
        ftk::ui::row("Opacity", [&] { edit |= ftk::ui::slider("##opacity", sp.opacity, 0.0f, 1.0f); });
        ftk::ui::row("Pixels per unit", "How many texture pixels make one metre. 0 sizes the sprite by the node's scale instead.",
                [&] {
                    edit |= ftk::ui::drag("##ppu", sp.pixels_per_unit,
                                     {.speed = 1.0f, .min = 0.0f, .max = 1000.0f, .format = "%.0f"});
                });
        ftk::ui::row("Flat", "A flat quad in the node's XY plane, instead of a billboard facing the camera.",
                [&] { edit |= ftk::ui::checkbox("##flat", sp.flat); });
        ftk::ui::row("Flip horizontally", [&] { edit |= ftk::ui::checkbox("##flip", sp.flip_h); });
        ftk::ui::row("Pixel art", "Sampled without smoothing, so pixels stay square.", [&] {
            const ftk::ui::Edit e = ftk::ui::checkbox("##pixel_art", sp.pixel_art);
            if (e.changed) sp.texture_loaded = false;
            edit |= e;
        });
        ftk::ui::row("Sort layer", "Which sprites draw in front: a higher layer is in front of a lower one.",
                [&] { edit |= ftk::ui::drag_int("##layer", sp.sort_layer, 0.2f); });
        ftk::ui::row("Sort order", "The order within a layer.", [&] { edit |= ftk::ui::drag_int("##order", sp.sort_order, 0.2f); });
    }
    edit |= lighting(sp, sp.normal_loaded, ctx);
    return edit;
}

ftk::ui::Edit draw_tilemap(Node3D& /*node*/, void* component, const InspectorContext& ctx) {
    auto& tm = *static_cast<TilemapComponent*>(component);
    static const std::string texture_exts[] = {".png", ".jpg", ".jpeg"};
    ftk::ui::Edit edit;
    bool reshaped = false;
    if (auto t = ftk::ui::PropertyTable("##tilemap")) {
        ftk::ui::row("Tileset", [&] {
            if (asset_slot({.label = "Tileset", .id = "##tileset", .extensions = texture_exts, .ctx = &ctx},
                           tm.tileset_texture)) {
                tm.texture_loaded = false;
                reshaped = true;
                edit = {true, true};
            }
        });
        ftk::ui::row("Tile size", [&] {
            const ftk::ui::Edit e = ftk::ui::ivec2("##tile_size", tm.tile_size, 1.0f, 1, 512, ftk::ui::Unit::Pixels);
            reshaped = reshaped || e.changed;
            edit |= e;
        });
        ftk::ui::row("Map size", "In tiles. Growing the map adds empty tiles; shrinking it drops the tiles outside.",
                [&] {
                    const ftk::ui::Edit e = ftk::ui::ivec2("##map_size", tm.map_size, 1.0f, 1, 256);
                    if (e.changed) {
                        tm.tiles.resize(static_cast<size_t>(tm.map_size.x) * tm.map_size.y, -1);
                        reshaped = true;
                    }
                    edit |= e;
                });
        ftk::ui::row("", [&] {
            if (ftk::ui::action(icon::add, "Fill with the first tile")) {
                tm.tiles.assign(static_cast<size_t>(tm.map_size.x) * tm.map_size.y, 0);
                reshaped = true;
                edit = {true, true};
            }
        });
        ftk::ui::row("Tile world size", [&] {
            const ftk::ui::Edit e = ftk::ui::drag("##world_size", tm.world_tile_size,
                                        {.speed = 0.01f, .min = 0.01f, .max = 10.0f, .unit = ftk::ui::Unit::Metres});
            reshaped = reshaped || e.changed;
            edit |= e;
        });
        ftk::ui::row("Pixel art", "Sampled without smoothing, so pixels stay square.", [&] {
            const ftk::ui::Edit e = ftk::ui::checkbox("##pixel_art", tm.pixel_art);
            if (e.changed) tm.texture_loaded = false;
            edit |= e;
        });
        ftk::ui::row("Sort layer", "Which layer the map draws in; below sprites by default.",
                [&] { edit |= ftk::ui::drag_int("##layer", tm.sort_layer, 0.2f); });
    }
    if (reshaped) tm.dirty = true;
    edit |= lighting(tm, tm.normal_loaded, ctx);
    return edit;
}

} // namespace fjell
