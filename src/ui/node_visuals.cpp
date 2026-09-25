#include "ui/node_visuals.hpp"

#include "renderer/components/mesh_component.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"
#include "ui/kit/icons.hpp"

namespace fjell {

NodeVisuals node_visuals(const Node& node) {
    using C = theme::Category;
    if (dynamic_cast<const DirectionalLightNode*>(&node)) return {ui::icon::directional_light, C::Light};
    if (dynamic_cast<const PointLightNode*>(&node)) return {ui::icon::point_light, C::Light};
    if (dynamic_cast<const SpotLightNode*>(&node)) return {ui::icon::spot_light, C::Light};
    if (dynamic_cast<const VolumetricFogNode*>(&node)) return {ui::icon::volumetric_fog, C::Environment};
    if (dynamic_cast<const FogVolumeNode*>(&node)) return {ui::icon::fog_volume, C::Environment};
    if (dynamic_cast<const PostProcessVolumeNode*>(&node)) return {ui::icon::post_process_volume, C::Environment};
    if (dynamic_cast<const WorldEnvironmentNode*>(&node)) return {ui::icon::world_environment, C::Environment};
    if (dynamic_cast<const CameraNode*>(&node)) return {ui::icon::camera, C::Camera};
    if (!node.preset_source.empty()) return {ui::icon::preset, C::Structure};
    if (!node.is_spatial()) return {ui::icon::folder, C::Structure};
    if (node.components.get<MeshComponent>() != nullptr) return {ui::icon::mesh, std::nullopt};
    return {ui::icon::transform, std::nullopt};
}

} // namespace fjell
