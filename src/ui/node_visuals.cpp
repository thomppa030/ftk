#include "ui/node_visuals.hpp"

#include "ui/categories.hpp"
#include "ui/icons.hpp"
#include "renderer/components/mesh_component.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"

namespace fjell {

NodeVisuals node_visuals(const Node& node) {
    using C = Category;
    if (dynamic_cast<const DirectionalLightNode*>(&node)) return {icon::directional_light, C::Light};
    if (dynamic_cast<const PointLightNode*>(&node)) return {icon::point_light, C::Light};
    if (dynamic_cast<const SpotLightNode*>(&node)) return {icon::spot_light, C::Light};
    if (dynamic_cast<const VolumetricFogNode*>(&node)) return {icon::volumetric_fog, C::Environment};
    if (dynamic_cast<const FogVolumeNode*>(&node)) return {icon::fog_volume, C::Environment};
    if (dynamic_cast<const PostProcessVolumeNode*>(&node)) return {icon::post_process_volume, C::Environment};
    if (dynamic_cast<const WorldEnvironmentNode*>(&node)) return {icon::world_environment, C::Environment};
    if (dynamic_cast<const CameraNode*>(&node)) return {icon::camera, C::Camera};
    if (!node.preset_source.empty()) return {icon::preset, C::Structure};
    if (!node.is_spatial()) return {icon::folder, C::Structure};
    if (node.components.get<MeshComponent>() != nullptr) return {icon::mesh, std::nullopt};
    return {icon::transform, std::nullopt};
}

} // namespace fjell
