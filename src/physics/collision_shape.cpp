#include "physics/collision_shape.hpp"

#include "renderer/resources/model.hpp"

namespace fjell {

CollisionMesh collision_mesh_of(const Model& model) {
    CollisionMesh mesh;
    mesh.vertices = &model.collision_vertices();
    mesh.indices = &model.collision_indices();
    mesh.source = model.collision_source();
    return mesh;
}

} // namespace fjell
