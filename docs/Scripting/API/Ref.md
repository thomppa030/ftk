# fjell::Ref&lt;T&gt;

A reference to a node, `Ref<Node3D>`, or to the component of type `T` on a node, which
knows when it is gone.

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
    Ref(const Node3D& node);   // explicit unless T is Node3D

    T*                     operator->() const;
    T&                     operator*()  const;
    [[nodiscard]] T*       get()     const;
    explicit operator bool()         const;
    [[nodiscard]] Ref<Node3D> node() const;
    [[nodiscard]] uint32_t node_id() const;
    bool operator==(const Ref&)      const;
};

}
```

`T` is [`Node3D`](Node3D.md) for the node itself, or a component type: an engine
component such as `RigidbodyComponent` or `ColliderComponent`, or any script component
declared with `FJELL_COMPONENT`.

## Remarks

Hold it as a `[[fj::property]]` and pick the node in the inspector. The picker offers
the nodes that carry a `T` (any 3D node for a `Ref<Node3D>`), takes a node dragged from
the hierarchy if it fits, and says why when it does not. The scene file saves the node
alone. The engine hands them out too: [`scene::find_node`](scene_api.md#scenefind_node),
[`scene::find<T>`](scene_api.md#scenefind), [`scene::spawn`](scene_api.md) and
[`get_component`](Node3D.md#node3dget_component) all give a `Ref`.

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

**Using a Ref whose target is gone stops the game.** `->` and `*` reach the target
directly, with no check to write. If the target is not there, the game stops on the
spot, shipped builds included, and says which script, which hook, which field and which
node: in the editor play ends with the script's node selected, a functional test fails
with the message, and a shipped game writes `crash_report.txt` beside its executable and
closes. Catching the `fjell::MissingReference` it throws does not keep the game running. So use
`->` for what is always there, and check with `if (ref)` or `get()` where absence is
expected, as for an enemy that can be killed:

```cpp
[[fj::property]] fjell::Ref<PlayerStats> player;   // always there
[[fj::property]] fjell::Ref<Enemy> target;          // may be killed

void tick(float) override {
    player->stamina += 1.0f;
    if (auto* enemy = target.get()) chase(*enemy);
}
```

A destructor cannot let an exception out, so use `get()` there, never `->`.

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
| [`Ref(node_id)`](#refref) | Refer to the `T` on a node, by id |
| [`Ref(node)`](#refref-node) | Refer to the `T` on a node, or to the node |
| [`operator->`, `operator*`](#refoperator-) | The target; stops the game when it is gone |
| [`get`](#refget) | The target, or null |
| [`operator bool`](#refoperator-bool) | Whether the target is there |
| [`node`](#refnode) | The node the target sits on |
| [`node_id`](#refnode_id) | The node it refers to |
| [`operator==`](#refoperator) | The same node in the same life |

---

## Ref::Ref

```cpp
explicit Ref(uint32_t node_id);
```

Refers to the `T` on node `node_id` as that node is now. The node need not carry a `T`
yet: the reference finds nothing until it does. For the engine calls that still hand out
node ids rather than `Ref`s.

---

## Ref::Ref (node) <a id="refref-node"></a>

```cpp
Ref(const Node3D& node);   // explicit unless T is Node3D
```

Refers to the `T` on `node`. A `Ref<Node3D>` made this way is the node itself, and the
conversion is implicit, so a script passes its own node wherever a `Ref<Node3D>` is taken:

```cpp
fjell::scene::spawn(lantern, pos, self());
```

---

## Ref::operator->

```cpp
T* operator->() const;
T& operator*()  const;
```

The target. When it is gone the game stops, naming the script, the hook, the field and
the node (see Remarks). The pointer is good for the rest of the call you are in.

---

## Ref::get

```cpp
[[nodiscard]] T* get() const;
```

The target, or null when there is none: never set, gone, or the node carries no `T` (for
a `Ref<Node3D>`, is a folder rather than a 3D node).
The pointer is good for the rest of the call you are in; ask again next frame rather than
keeping it.

---

## Ref::operator bool

```cpp
explicit operator bool() const;
```

Whether the target is there now: the same as `get() != nullptr`.

---

## Ref::node

```cpp
[[nodiscard]] Ref<Node3D> node() const;
```

The node the target sits on, as a `Ref<Node3D>`: gone when that node is, whether or not it
still carries a `T`. For a `Ref<Node3D>`, the same node.

```cpp
auto stats = fjell::scene::find<PlayerStats>();
stats.node()->position();
```

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
- [`Node3D`](Node3D.md) — what a node offers
