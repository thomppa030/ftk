#include "ui/node_visuals.hpp"

#include "ftk/ui/kit/icons.hpp"
#include "renderer/components/mesh_component.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"

namespace fjell {

NodeVisuals node_visuals(const Node& node) {
    using C = ftk::theme::Category;
    if (dynamic_cast<const DirectionalLightNode*>(&node)) return {ftk::ui::icon::directional_light, C::Light};
    if (dynamic_cast<const PointLightNode*>(&node)) return {ftk::ui::icon::point_light, C::Light};
    if (dynamic_cast<const SpotLightNode*>(&node)) return {ftk::ui::icon::spot_light, C::Light};
    if (dynamic_cast<const VolumetricFogNode*>(&node)) return {ftk::ui::icon::volumetric_fog, C::Environment};
    if (dynamic_cast<const FogVolumeNode*>(&node)) return {ftk::ui::icon::fog_volume, C::Environment};
    if (dynamic_cast<const PostProcessVolumeNode*>(&node)) return {ftk::ui::icon::post_process_volume, C::Environment};
    if (dynamic_cast<const WorldEnvironmentNode*>(&node)) return {ftk::ui::icon::world_environment, C::Environment};
    if (dynamic_cast<const CameraNode*>(&node)) return {ftk::ui::icon::camera, C::Camera};
    if (!node.preset_source.empty()) return {ftk::ui::icon::preset, C::Structure};
    if (!node.is_spatial()) return {ftk::ui::icon::folder, C::Structure};
    if (node.components.get<MeshComponent>() != nullptr) return {ftk::ui::icon::mesh, std::nullopt};
    return {ftk::ui::icon::transform, std::nullopt};
}

} // namespace fjell
