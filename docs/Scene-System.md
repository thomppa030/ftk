# Scene System

The scene is a flat list of `RenderObject`s with optional parent-child relationships. There's no deep tree — parenting is tracked with `parent_id`, and world matrices are computed top-down each frame.

## RenderObject

Every object in the scene is a `RenderObject`. It holds everything needed to place, render, and simulate something:

| Field | What it does |
|---|---|
| `id` | Unique identifier within the scene |
| `parent_id` | ID of the parent object, or `-1` for root objects |
| `world_matrix_` | Computed each frame from the hierarchy, from where each node is drawn — for drawing only |
| `model_handle` / `material_handle` | Bindless resource handles into `ResourceManager` |
| `model` / `material` | Resolved raw pointers (cached after handle lookup) |
| `transform` | Local-space position, rotation, scale — the simulation's exact state |
| `interpolated_transform_` | Where a node the last fixed step moved is drawn: its transform blended between that step's start and end |
| `name` | Display name (shown in hierarchy panel) |
| `source` | Where this object came from (e.g. a glTF file path) |
| `material_name` | Material identifier from the source asset |
| `actor` | Optional scripting actor attached to this object |
| `components` | List of script components attached to this object |
| `rigidbody` | Optional rigidbody description for physics |
| `collider` | Optional collider description for physics |
| `emitter` | Optional particle emitter |

## Transform

Transforms use **quaternions** for rotation, not Euler angles.

```cpp
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // identity
    glm::vec3 scale{1.0f};
};
```

- `matrix()` — builds the local TRS matrix.
- `euler_degrees()` — returns the rotation as Euler angles in degrees (for UI display).
- `set_euler_degrees(degrees)` — sets the quaternion from Euler angles in degrees (for UI input).

Quaternions avoid gimbal lock and make interpolation reliable. The Euler helpers exist so the properties panel can show human-readable angles.

## Scene Operations

**Object management:**
- `add` — append an object to the scene.
- `remove` — delete an object by ID.
- `extract` — remove an object and return it (used by undo).
- `restore` — re-insert a previously extracted object (used by redo).
- `clear` — wipe everything.

**Lookup:**
- `find` — get an object by ID.
- `index_of` — get the position of an object in the flat list.

**Hierarchy:**
- `reparent` — move an object under a new parent. World position is preserved — the local transform is adjusted so the object doesn't visually jump.
- `compute_world_matrices` — walks the hierarchy top-down and writes each object's `world_matrix_`, from `interpolated_transform_` for a node the last fixed step moved and from `transform` otherwise. Called once per frame before rendering.
- `current_world_matrix` — a node's world matrix from the transforms as they are now, through its parents. What the simulation reads: `world_matrix_` is the drawn placement and trails the simulation by up to a step.
- `teleport` — move a node to a world placement at once, with no blend, and its physics body or character with it.

**Fixed steps:** `begin_fixed_step` keeps every node's transform before a step, `end_fixed_steps` notes what the last step moved, and `interpolate(alpha)` blends those nodes for drawing (position, rotation and scale each only while still where the step left it). See [Main Loop](Main-Loop.md#the-fixed-step).

**Lights:**
- Point and spot lights are managed separately from the object list.
- `add` — append a light.
- `insert` — add a light at a specific index.
- `remove` — remove a light at a specific index.

**Serialization:**
- `save` / `load` — JSON format. Stores transforms, hierarchy, resource references, physics descriptors, components, and lights.

**Snapshotting:**
- `clone_flat()` on `RenderObject` produces a shallow copy suitable for undo/redo snapshots. It captures the object's current state without deep-copying GPU resources.

## Undo/Redo

The scene doesn't own the undo stack — that lives in `CommandHistory`. But `extract`, `restore`, and `clone_flat` exist specifically to support it. A typical undo flow:

1. Before a destructive edit, `clone_flat()` the affected object.
2. Apply the edit.
3. Push a command to `CommandHistory` that holds the snapshot.
4. On undo, `extract` the modified object and `restore` the snapshot (or vice versa).

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
      "rotation": [0, 0, 0, 1],
      "scale": [10, 1, 10]
    }
  ]
}
```

On load, `source` determines how the model is created:
- `"primitive:cube"` / `"primitive:plane"` / `"primitive:sphere"` — generated geometry
- File path — loaded via fastgltf

Handles are resolved from the source/material strings during load. Component data is stored as raw JSON and deserialized by `ScriptManager` after the .so is loaded.
