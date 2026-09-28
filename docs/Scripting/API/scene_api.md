# fjell::scene

Finds, spawns, and destroys scene nodes.

| | |
|---|---|
| Header | `fjell/scene_api.hpp` |
| Source | `src/scripting/scene_api.cpp` |

## Syntax

```cpp
namespace fjell {
namespace scene {
    uint32_t find_node(const char* name);
    uint32_t spawn    (const char* name, const glm::vec3& pos = glm::vec3{0.0f});
    uint32_t spawn    (const Asset<Preset>& preset,
                       const glm::vec3& pos = glm::vec3{0.0f},
                       int32_t parent_id = -1);
}
}
```

## Remarks

`scene` is the global access point for the scene graph. For *operations on a known node* (read/write its transform, get a component), use [`Node3D`](Node3D.md) — wrap an ID via `Node3D{id}` or get yours via `self()` on a [`ScriptComponent`](ScriptComponent.md).

Node IDs are `uint32_t`. ID `0` is reserved as the "invalid / null" sentinel — every call that returns an ID returns `0` on failure.

Names are **not** unique. `find_node` returns the first match (engine-defined order). To keep hold of what is on a specific node, hold a [`Ref<T>`](Ref.md) to its component instead of looking it up by name every frame.

## Functions

| Name | Description |
|---|---|
| [`find_node`](#scenefind_node) | Look up a node by name |
| [`spawn(name, pos)`](#scenespawn-empty) | Create an empty node |
| [`spawn(preset, pos, parent)`](#scenespawn-preset) | Instantiate a `.fjp` preset |

---

## scene::find_node

### Syntax
```cpp
uint32_t find_node(const char* name);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `name` | `const char*` | Node name to search for. Must not be null. Case-sensitive. |

### Return value
The node's ID, or `0` if no node matches.

### Remarks
Linear search over the scene. Cheap for small scenes; for hot paths in large scenes, cache the ID after the first lookup or hold a [`Ref<T>`](Ref.md).

### Example
```cpp
uint32_t cam = fjell::scene::find_node("MainCamera");
if (cam != 0) {
    fjell::camera::set_active(cam);
}
```

---

## scene::spawn (empty)

### Syntax
```cpp
uint32_t spawn(const char* name, const glm::vec3& pos = glm::vec3{0.0f});
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `name` | `const char*` | Name to assign the new node. Need not be unique. |
| `pos` | `glm::vec3` | Initial world position. Default origin. |

### Return value
The new node's ID. Always non-zero on success.

### Remarks
The new node has no components, no parent (root-level), and identity rotation / scale. To add components, wrap the ID and use `Node3D::add_component<T>()`.

### Example
```cpp
uint32_t id = fjell::scene::spawn("Pickup", {0, 1, 0});
fjell::Node3D node{id};
node.add_component<CoinCollectible>();
```

---

## scene::spawn (preset)

### Syntax
```cpp
uint32_t spawn(const Asset<Preset>& preset,
               const glm::vec3& pos = glm::vec3{0.0f},
               int32_t parent_id = -1);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `preset` | `const Asset<Preset>&` | Preset reference (assigned in the inspector). One that is unset or failed to load stops the game (see [`Asset`](Asset.md)). |
| `pos` | `glm::vec3` | World position for the preset's root node. Default origin. |
| `parent_id` | `int32_t` | Parent node ID. `-1` (default) spawns at root. |

### Return value
The ID of the spawned preset's **root node**, or `0` if the scene is unavailable.

### Remarks
Instantiates the full preset hierarchy — child nodes, components, default field values are all created. The returned ID is just the root; walk children via `Node3D` or hold a [`Ref<T>`](Ref.md) to a component on a descendant if you need one.

Spawning under a parent inherits the parent's transform; `pos` is in *world* space, applied to the root after parenting.

### Example
```cpp
class Spawner : public fjell::ScriptComponent {
    FJELL_COMPONENT(Spawner)

    [[fj::property]] fjell::Asset<fjell::Preset> enemy;

    void spawn_one() {
        auto pos = transform().position + glm::vec3{5, 0, 0};
        uint32_t id = fjell::scene::spawn(enemy, pos);
        if (id == 0) fjell::Log::warn("enemy preset not assigned");
    }
}
```

## See also
- [`Node3D`](Node3D.md) — per-node operations
- [`Asset`](Asset.md) — how `Asset<Preset>` is resolved
- [`Ref`](Ref.md) — references to components on spawned nodes, which know when they are gone
