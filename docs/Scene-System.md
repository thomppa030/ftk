# Scene System

## RenderObject

Every entity in the scene is a `RenderObject`:

```cpp
struct RenderObject {
    uint32_t id;                // Unique, auto-incremented
    int32_t parent_id;          // -1 = root
    glm::mat4 world_matrix_;   // Computed each frame

    Model* model;               // Mesh geometry (owned by ResourceManager)
    Material* material;         // PBR material (owned by ResourceManager)
    Transform transform;        // Local TRS

    std::string name;           // Display name
    std::string source;         // "primitive:cube", "assets/models/thing.glb", etc.
    std::string material_name;

    std::vector<ComponentInstance> components;   // Script components

    std::optional<RigidbodyDesc> rigidbody;     // Physics body descriptor
    std::optional<ColliderDesc> collider;        // Physics collider descriptor
};
```

## Transform

Local transform relative to parent (or world if root):

```cpp
struct Transform {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};  // Euler angles in degrees (XYZ order)
    glm::vec3 scale{1.0f};
};
```

`Transform::matrix()` builds the TRS matrix: translate, then rotate X→Y→Z, then scale. `decompose_matrix()` reverses this.

## Hierarchy

Objects reference their parent via `parent_id`. The hierarchy is stored flat — all objects live in a single `std::vector<RenderObject>`.

`compute_world_matrices()` runs every frame, propagating transforms top-down:
1. Roots (parent_id == -1) get `world_matrix = local.matrix()`
2. Children get `world_matrix = parent.world_matrix * local.matrix()`
3. Cycle detection breaks infinite loops by orphaning offenders

`reparent(child_id, new_parent_id)` adjusts the child's local transform so its world position doesn't change.

## Scene Operations

| Method | Description |
|--------|-------------|
| `add(model, material, transform, name, source)` | Create object, returns ID |
| `remove(id)` | Delete object, orphan its children |
| `find(id)` | Lookup by ID (returns pointer or null) |
| `clear()` | Remove all objects |
| `reparent(child, parent)` | Change parent, preserve world position |
| `render(cmd, layout, stats)` | Draw all objects with push constants |
| `save(path)` | Serialize to JSON |
| `Scene::load(path, resources, project)` | Deserialize from JSON |

## Serialization Format

```json
{
  "next_id": 4,
  "objects": [
    {
      "id": 1,
      "parent_id": -1,
      "name": "Ground",
      "source": "primitive:plane",
      "material": "default",
      "position": [0, 0, 0],
      "rotation": [0, 0, 0],
      "scale": [10, 1, 10],
      "collider": {
        "shape": 0,
        "half_extents": [0.5, 0.5, 0.5],
        "radius": 0.5,
        "height": 1.0,
        "offset": [0, 0, 0]
      },
      "components": [
        {
          "type": "PlayerController",
          "data": { "speed": 5.0, "jump_force": 8.0 }
        }
      ]
    }
  ]
}
```

On load, `source` determines how the model is created:
- `"primitive:cube"` / `"primitive:plane"` / `"primitive:sphere"` — generated geometry
- File path — loaded via fastgltf

Component data is stored as raw JSON. The `ScriptManager` instantiates components and deserializes the data after the .so is loaded.
