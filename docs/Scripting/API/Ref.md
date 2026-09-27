# fjell::Ref&lt;T&gt;

A reference to the component of type `T` on a node, which knows when it is gone.

| | |
|---|---|
| Header | `fjell/ref.hpp` |
| Source | (header-only; resolved by `Scene::resolve()` through `src/scripting/scene_api.cpp`) |

## Syntax

```cpp
namespace fjell {

template <typename T>
class Ref {
public:
    Ref() = default;
    explicit Ref(uint32_t node_id);

    [[nodiscard]] T*       get()     const;
    explicit operator bool()         const;
    [[nodiscard]] uint32_t node_id() const;
    bool operator==(const Ref&)      const;
};

}
```

`T` is a component type: an engine component such as `RigidbodyComponent` or
`ColliderComponent`, or any script component declared with `FJELL_COMPONENT`.

## Remarks

Hold it as a `[[fj::property]]` and pick the node in the inspector. The picker offers
the nodes that carry a `T`, takes a node dragged from the hierarchy if it carries one,
and says why when it does not. The scene file saves the node alone.

```cpp
class Hunter : public fjell::ScriptComponent {
    FJELL_COMPONENT(Hunter)
public:
    [[fj::property]] fjell::Ref<PlayerStats> prey;

    void tick(float) override {
        if (auto* stats = prey.get()) chase(*stats);
    }
};
```

**It knows when its target is gone.** A `Ref` finds nothing once its node is destroyed
or taken out of the scene, once the node no longer carries a `T`, and while the game
library reloads. It never finds another node that later took the same id, after a scene
reload say: it holds the node's *generation*, which a node gets once and keeps for its
life. A node deleted in the editor and brought back by undo is the same node, and its
references find it again.

**It is cheap.** A `Ref` keeps what it found until anything, anywhere, goes away (a
destroy, a removed component, a reload) and only then asks the engine again, once, on its
next use. Components never move while they live, so what it kept stays right until then.

## Functions

| Name | Description |
|---|---|
| [`Ref(node_id)`](#refref) | Refer to the `T` on a node |
| [`get`](#refget) | The target, or null |
| [`operator bool`](#refoperator-bool) | Whether the target is there |
| [`node_id`](#refnode_id) | The node it refers to |
| [`operator==`](#refoperator) | The same node in the same life |

---

## Ref::Ref

```cpp
explicit Ref(uint32_t node_id);
```

Refers to the `T` on node `node_id` as that node is now. The node need not carry a `T`
yet: the reference finds nothing until it does.

---

## Ref::get

```cpp
[[nodiscard]] T* get() const;
```

The target, or null when there is none: never set, gone, or the node carries no `T`.
The pointer is good for the rest of the call you are in; ask again next frame rather than
keeping it.

---

## Ref::operator bool

```cpp
explicit operator bool() const;
```

Whether the target is there now: the same as `get() != nullptr`.

---

## Ref::node_id

```cpp
[[nodiscard]] uint32_t node_id() const;
```

The node it refers to, 0 when never set. Not a check that the node is still there.

---

## Ref::operator==

```cpp
bool operator==(const Ref& other) const;
```

True when both refer to the same node in the same life.

## See also

- [`Asset`](Asset.md) — references to engine assets: presets, sounds, materials
- [`Node3D`](Node3D.md) — the node handle
