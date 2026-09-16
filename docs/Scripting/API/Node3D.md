# fjell::Node3D

Lightweight handle to a scene node. Wraps a 32-bit node ID; cheap to copy and store.

| | |
|---|---|
| Header | `fjell/node.hpp` |
| Source | (header-only; engine functions live in `src/scripting/scene_api.cpp`) |

## Syntax

```cpp
namespace fjell {

struct Node3D {
    uint32_t node_id{0};

    Node3D() = default;
    explicit Node3D(uint32_t id);

    [[nodiscard]] uint32_t id()    const;
    [[nodiscard]] bool     valid() const;

    [[nodiscard]] glm::vec3& position() const;
    [[nodiscard]] glm::vec3& scale()    const;
    [[nodiscard]] glm::quat& rotation() const;

    template <typename T> [[nodiscard]] T* get_component() const;
    template <typename T>               T* add_component() const;

    void destroy() const;
};

}
```

## Remarks

`Node3D` is a value type — copying it is just copying a `uint32_t`. The underlying node lives in the engine; the handle doesn't extend its lifetime. Use [`WeakRef<T>`](WeakRef.md) when you need generational safety (an ID that was reused by a different node returns the wrong object).

`valid()` distinguishes the null/default handle (`id == 0`) from a real one. It does **not** check whether the node still exists in the scene — for that, look up via [`SceneAPI::find_node`](SceneAPI.md#sceneapifind_node) or wrap the ID in a `WeakRef`.

The transform accessors return references into engine memory. They're safe to write through within a frame; don't hold the reference across ticks.

## Members

| Name | Type | Description |
|---|---|---|
| `node_id` | `uint32_t` | The wrapped ID. `0` means null. |

## Constructors

| Name | Description |
|---|---|
| `Node3D()` | Null handle (`node_id == 0`). |
| `Node3D(uint32_t id)` | Wraps an existing ID. Does not validate. |

## Functions

| Name | Description |
|---|---|
| [`id`](#node3did) | The wrapped node ID |
| [`valid`](#node3dvalid) | `true` if `node_id != 0` |
| [`position`](#node3dposition) | World position (reference) |
| [`scale`](#node3dscale) | Scale (reference) |
| [`rotation`](#node3drotation) | Rotation (reference) |
| [`get_component`](#node3dget_component) | Look up a component by type |
| [`add_component`](#node3dadd_component) | Add a component to the node |
| [`destroy`](#node3ddestroy) | Remove the node from the scene |

---

## Node3D::id

### Syntax
```cpp
[[nodiscard]] uint32_t id() const;
```

### Return value
The wrapped ID. Same as the `node_id` field.

---

## Node3D::valid

### Syntax
```cpp
[[nodiscard]] bool valid() const;
```

### Return value
`true` if `node_id != 0`.

### Remarks
A cheap structural check. Does **not** verify that the node still exists in the scene. For generational safety use [`WeakRef`](WeakRef.md).

---

## Node3D::position

### Syntax
```cpp
[[nodiscard]] glm::vec3& position() const;
```

### Return value
Reference to the node's position in world space.

### Remarks
Writeable — assigning to the returned reference mutates the node's transform directly. The change is visible to other systems the next time they read the transform (typically the next frame).

The `const` on the method refers to the *handle* being const, not the underlying transform. The handle is just an ID.

### Example
```cpp
node.position() += glm::vec3{0, 1, 0} * dt;
node.position() = target_pos;
```

---

## Node3D::scale

### Syntax
```cpp
[[nodiscard]] glm::vec3& scale() const;
```

### Return value
Reference to the node's scale. Same semantics as `position()`.

---

## Node3D::rotation

### Syntax
```cpp
[[nodiscard]] glm::quat& rotation() const;
```

### Return value
Reference to the node's rotation quaternion. Same semantics as `position()`.

### Example
```cpp
node.rotation() = glm::angleAxis(glm::radians(90.0f), glm::vec3{0, 1, 0});
```

---

## Node3D::get_component

### Syntax
```cpp
template <typename T>
[[nodiscard]] T* get_component() const;
```

### Template parameters
| Name | Description |
|---|---|
| `T` | Component type. Must have a `static constexpr const char* type_name` member — provided automatically by `FJELL_COMPONENT(T)`. |

### Return value
Pointer to the component instance on this node, or `nullptr` if the node doesn't have a `T`.

### Remarks
Looks up the component on the engine side by `type_name`. Cheap (hash lookup). Result is valid for the current frame; don't store across reloads.

For engine components (`MeshComponent`, `SpriteComponent`, `RigidbodyComponent`, etc.) use the types from `fjell/engine_components.hpp`.

### Example
```cpp
auto* mesh = node.get_component<fjell::MeshComponent>();
if (mesh) { /* ... */ }

auto* player = node.get_component<PlayerController>();
```

---

## Node3D::add_component

### Syntax
```cpp
template <typename T>
T* add_component() const;
```

### Template parameters
| Name | Description |
|---|---|
| `T` | Component type with `static constexpr const char* type_name`. |

### Return value
Pointer to the newly added component, or `nullptr` if creation failed (e.g. `T` isn't registered).

### Remarks
Calls `begin_play()` on the new component before returning. Subsequent `tick`s include it.

Adding a duplicate of an already-present component is allowed at the API level but is rarely what you want.

### Example
```cpp
uint32_t id = fjell::SceneAPI::spawn("Pickup", pos);
fjell::Node3D node{id};
auto* coin = node.add_component<Coin>();
```

---

## Node3D::destroy

### Syntax
```cpp
void destroy() const;
```

### Remarks
Marks the node for destruction. The node, its components, and its children are removed at the next safe point (typically end-of-frame).

After calling `destroy`, the handle itself remains a valid `Node3D` value (the ID is still set), but `get_component` and the transform accessors are unsafe to call. Discard the handle.

Calling `destroy` on the node a `ScriptComponent` is attached to — including via `self().destroy()` — is allowed. The component's `end_play()` will be called before destruction completes.

### Example
```cpp
void on_trigger_enter(uint32_t /*other*/) override {
    self().destroy();
}
```

## Example

```cpp
// Find a node, modify it, look up a component
uint32_t id = fjell::SceneAPI::find_node("Sun");
if (id != 0) {
    fjell::Node3D sun{id};
    sun.rotation() = glm::angleAxis(angle, glm::vec3{1, 0, 0});

    if (auto* light = sun.get_component<DirectionalLight>()) {
        light->intensity = 1.5f;
    }
}
```

## See also
- [`SceneAPI`](SceneAPI.md) — finding and spawning nodes
- [`ScriptComponent::self`](ScriptComponent.md#scriptcomponentself) — your own node's handle
- [`WeakRef`](WeakRef.md) — persistent, generation-checked node references
