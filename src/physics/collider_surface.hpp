#pragma once

namespace fjell {

class Project;
class ResourceManager;
struct ColliderComponent;

/// Resolves the collider's surface path to the handle physics reports, or
/// clears it when the path is empty or doesn't load. Called when a scene is
/// loaded and when the path is changed in the editor.
void load_collider_surface(ColliderComponent& col, ResourceManager* resources,
                           const Project* project);

} // namespace fjell
