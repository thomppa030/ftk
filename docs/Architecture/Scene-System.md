# Scene System

> Stub — fill in.

## Scope
The runtime scene graph: nodes, components, transforms, asset references. Hot-reload-safe by design — script-side code talks to scene objects via opaque handles, not raw pointers.

## Planned sections
- `SceneNode` — transform, parent/children, components
- Components
  - `MeshComponent`, `AnimatorComponent`, `InputComponent`, `UiComponent`, etc.
  - Component registry
- `Node3D` — script-side opaque handle
- Transform hierarchy
  - Local vs world
  - Dirty flag propagation (O(n) batched update)
- `AssetDatabase` — UUID-based asset lookups
- `AssetId` and `AssetRef<T>`
- `.fjscene` format
- Serialization round-trip
- Find / iterate APIs
- Lifetime — when does a node go away, who owns it
