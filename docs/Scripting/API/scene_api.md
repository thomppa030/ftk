# fjell::scene

Finds and spawns nodes in the running scene.

| | |
|---|---|
| Header | `fjell/scene_api.hpp` |
| Source | `src/scripting/scene_api.cpp` |

## Syntax

```cpp
namespace fjell {
namespace scene {
    Ref<Node3D> find_node(const char* name);
    template <typename T> Ref<T> find();

    Ref<Node3D> spawn(const char* name, const glm::vec3& pos = glm::vec3{0.0f});
    Ref<Node3D> spawn(const Asset<Preset>& preset, const glm::vec3& pos = glm::vec3{0.0f});
    Ref<Node3D> spawn(const Asset<Preset>& preset, const glm::vec3& pos, const Ref<Node3D>& parent);
}
}
```

## Remarks

Everything here hands back a [`Ref`](Ref.md): to a node, or to the component you looked for.
What you do with a node once you have it is on [`Node3D`](Node3D.md).

Prefer a `[[fj::property]] Ref<T>` picked in the inspector over a lookup: it names exactly
the node meant. Of the lookups, `find<T>()` finds what you are after by what it is, where
`find_node` depends on how the scene happens to name things.

A node spawned here is usable at once, in the same step, although it joins the scene's
walks only after the frame's scripts have run.

## Functions

| Name | Description |
|---|---|
| [`find_node`](#scenefind_node) | A node by name |
| [`find<T>`](#scenefind) | The component of type `T`, wherever it is |
| [`spawn(name, pos)`](#scenespawn-empty) | Create an empty node |
| [`spawn(preset, pos[, parent])`](#scenespawn-preset) | Instantiate a `.fjp` preset |

---

## scene::find_node

```cpp
Ref<Node3D> find_node(const char* name);
```

The first node called `name` (case-sensitive), or an empty `Ref` when there is none. Names
are not unique, and the first match wins. A linear search over the scene: look up once,
in `begin_play()`, and keep the `Ref`.

```cpp
if (auto sun = fjell::scene::find_node("Sun")) {
    sun->rotation() = glm::angleAxis(angle, glm::vec3{1, 0, 0});
}
```

---

## scene::find

```cpp
template <typename T>
Ref<T> find();
```

The `T` on whichever node carries one, wherever it sits in the hierarchy and whatever the
node is called; an empty `Ref` when none does. When several do, the one that has been in
the scene longest. `T` is a script component or an engine component.

Cheap: the engine keeps every `T` in the scene together and answers from there. The node
it sits on is `.node()`:

```cpp
auto stats = fjell::scene::find<PlayerStats>();
stats->hp = stats->max_hp;
auto player = stats.node();
```

---

## scene::spawn (empty)

```cpp
Ref<Node3D> spawn(const char* name, const glm::vec3& pos = glm::vec3{0.0f});
```

| Name | Type | Description |
|---|---|---|
| `name` | `const char*` | Name for the new node. Need not be unique. |
| `pos` | `glm::vec3` | World position. Default origin. |

A new node with no components, at the root of the scene, with identity rotation and scale.
An empty `Ref` only when no scene is loaded.

```cpp
auto pickup = fjell::scene::spawn("Pickup", {0, 1, 0});
pickup->add_component<CoinCollectible>();
```

---

## scene::spawn (preset)

```cpp
Ref<Node3D> spawn(const Asset<Preset>& preset, const glm::vec3& pos = glm::vec3{0.0f});
Ref<Node3D> spawn(const Asset<Preset>& preset, const glm::vec3& pos, const Ref<Node3D>& parent);
```

| Name | Type | Description |
|---|---|---|
| `preset` | `const Asset<Preset>&` | Preset reference (assigned in the inspector). One that is unset or failed to load stops the game (see [`Asset`](Asset.md)). |
| `pos` | `glm::vec3` | World position for the preset's root node. Default origin. |
| `parent` | `const Ref<Node3D>&` | Node to spawn it under, which it then moves with. One that is gone stops the game. `self()` passes the script's own node. |

Instantiates the whole preset hierarchy — child nodes, components, property values — and
gives its root node; `find_child` on it reaches the rest. Without a parent it spawns at
the root of the scene. `pos` is in world space either way. A `Ref` between the preset's
own nodes refers to this copy's node (see [`Ref`](Ref.md#remarks)).

Single-player only: it creates the nodes locally. Use `net::spawn()` for anything that
must exist on every peer.

```cpp
class Spawner : public fjell::ScriptComponent {
    FJELL_COMPONENT(Spawner)

    [[fj::property]] fjell::Asset<fjell::Preset> enemy;

    void spawn_one() {
        auto spawned = fjell::scene::spawn(enemy, transform().position + glm::vec3{5, 0, 0});
        spawned->find_child("Weapon")->destroy();
    }
};
```

## See also

- [`Node3D`](Node3D.md) — what a node offers
- [`Ref`](Ref.md) — references to nodes and their components, which know when they are gone
- [`Asset`](Asset.md) — how `Asset<Preset>` is resolved
