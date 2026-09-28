# fjell::Node3D

A node in the scene, as a script sees it: the node itself, not a copy and not an id.

| | |
|---|---|
| Header | `fjell/node.hpp` |
| Source | (header-only; engine functions live in `src/scripting/scene_api.cpp`) |

## Syntax

```cpp
namespace fjell {

struct Node3D {
    Node3D() = delete;
    Node3D(const Node3D&) = delete;

    [[nodiscard]] glm::vec3& position();
    [[nodiscard]] glm::quat& rotation();
    [[nodiscard]] glm::vec3& scale();

    [[nodiscard]] Transform render_transform() const;
    void teleport(const glm::vec3& world_position, const glm::quat& world_rotation);

    template <typename T> [[nodiscard]] Ref<T> get_component() const;
    template <typename T>               Ref<T> add_component();

    [[nodiscard]] Ref<Node3D> find_child(const char* name) const;

    void destroy();
};

}
```

## Remarks

A script never makes a `Node3D`, and never holds one by value. It reaches its own node
with [`self()`](ScriptComponent.md#scriptcomponentself), which is there for as long as
the script is, and any other node through a [`Ref<Node3D>`](Ref.md), whose `->` stops the
game if that node is gone:

```cpp
[[fj::property]] fjell::Ref<fjell::Node3D> door;

void open() {
    self().position().y += 1.0f;   // your own node
    door->position().y += 2.0f;    // another one
}
```

Hold a `Ref<Node3D>` in a `[[fj::property]]` and pick the node in the inspector, or get one
from [`scene::find_node`](scene_api.md#scenefind_node), [`scene::spawn`](scene_api.md),
`find_child`, or `.node()` on a `Ref` to one of its components. A folder in the hierarchy
is not a 3D node: a `Ref<Node3D>` to one finds nothing.

The transform accessors hand back the node's own transform: writes apply at once.

## Functions

| Name | Description |
|---|---|
| [`position`](#node3dposition) | Local position, the simulation's state |
| [`rotation`](#node3drotation) | Local rotation |
| [`scale`](#node3dscale) | Local scale |
| [`render_transform`](#node3drender_transform) | Where the node is drawn this frame, in world space |
| [`teleport`](#node3dteleport) | Move the node somewhere at once, body and all |
| [`get_component`](#node3dget_component) | Refer to a component on it by type |
| [`add_component`](#node3dadd_component) | Add a component to it |
| [`find_child`](#node3dfind_child) | A direct child by name |
| [`destroy`](#node3ddestroy) | Remove the node from the scene |

---

## Node3D::position

```cpp
[[nodiscard]] glm::vec3& position();
```

The node's position relative to its parent. Writing through it moves the node.

This is the simulation's state. A node the fixed steps move — a physics body, a
character — is where the last step left it here, and drawn up to a step behind,
blended between the last two steps; to follow it on screen, read
[`render_transform`](#node3drender_transform). A dynamic body written here is
put back by physics at its next step; push it with forces or its velocity, or
move it with [`teleport`](#node3dteleport).

```cpp
door->position() += glm::vec3{0, 1, 0} * dt;
```

---

## Node3D::rotation

```cpp
[[nodiscard]] glm::quat& rotation();
```

The node's rotation relative to its parent, as `position()` is its position.

```cpp
door->rotation() = glm::angleAxis(glm::radians(90.0f), glm::vec3{0, 1, 0});
```

---

## Node3D::scale

```cpp
[[nodiscard]] glm::vec3& scale();
```

The node's scale relative to its parent, as `position()` is its position.

---

## Node3D::render_transform

```cpp
[[nodiscard]] Transform render_transform() const;
```

Where the node is drawn this frame, in world space: position, rotation and scale.

The simulation moves nodes in fixed steps, and each frame draws what the last step moved
blended between that step's start and end, so it moves smoothly at any frame rate. This is
that drawn placement; `position()` is the simulation's own, up to a step ahead. A camera,
marker or trail following a node reads this, from
[`late_tick`](ScriptComponent.md#scriptcomponentlate_tick), or it steps while the node
glides. Called from `tick()`, which runs before the steps, it is where the node was drawn
last frame.

```cpp
void late_tick(float /*dt*/) override {
    transform().position = target->render_transform().position + offset_;
}
```

---

## Node3D::teleport

```cpp
void teleport(const glm::vec3& world_position, const glm::quat& world_rotation);
```

| Name | Type | Description |
|---|---|---|
| `world_position` | `const glm::vec3&` | Where to put the node, in world space. |
| `world_rotation` | `const glm::quat&` | Its rotation, in world space. |

Moves the node at once: it is drawn there this frame rather than blended there from where
it was, and a physics body or character on it moves with it, keeping its velocity. Use it
for respawns, portals and resets. Writing `position()` moves a node too, but a dynamic
body's physics would put it back.

```cpp
self().teleport(spawn_point, glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
```

---

## Node3D::get_component

```cpp
template <typename T>
[[nodiscard]] Ref<T> get_component() const;
```

A [`Ref<T>`](Ref.md) to the node's `T`: an engine component (`MeshComponent`,
`RigidbodyComponent`, ... from `fjell/engine_components.hpp`) or a script component
declared with `FJELL_COMPONENT`. It reads as not there when the node has no `T`, and as
gone once the node or the component is, like any `Ref`, so it can be kept:

```cpp
if (auto body = self().get_component<fjell::RigidbodyComponent>()) {
    body->mass = 5.0f;
}
```

---

## Node3D::add_component

```cpp
template <typename T>
Ref<T> add_component();
```

Adds a `T` to the node and refers to it. While the game is running, the new script's
`begin_play()` runs before this returns.

A node holds one component of a type. When it already has a `T`, that one is kept,
unchanged and without a second `begin_play()`, and a warning is logged. The `Ref` reads
as not there when `T` is an engine component scripts cannot add, or the new script's own
`begin_play()` took it off again.

```cpp
auto pickup = fjell::scene::spawn("Pickup", pos);
pickup->add_component<CoinCollectible>()->spin_speed = 90.0f;
```

---

## Node3D::find_child

```cpp
[[nodiscard]] Ref<Node3D> find_child(const char* name) const;
```

The direct child called `name`, or an empty `Ref` when there is none. Only immediate
children are searched. Names repeat across instances — each spawned player has its own
"Camera" — and this finds the one under this node.

```cpp
auto camera = self().find_child("Camera");
```

---

## Node3D::destroy

```cpp
void destroy();
```

Queues the node for destruction along with its children. It is removed at the end of the
frame, not at once: it keeps ticking until then, and `Ref`s to it find it for the rest of
the frame. After that they read as gone.

Destroying a script's own node, `self().destroy()`, is allowed. The script's `end_play()`
runs before the node goes.

## See also

- [`Ref`](Ref.md) — references to nodes and their components, which know when they are gone
- [`scene`](scene_api.md) — finding and spawning nodes
- [`ScriptComponent::self`](ScriptComponent.md#scriptcomponentself) — a script's own node
