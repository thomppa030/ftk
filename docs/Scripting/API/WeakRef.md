# fjell::WeakRef&lt;T&gt;

Non-owning, generation-checked handle to a scene-side object. Safe across destroy-and-replace.

| | |
|---|---|
| Header | `fjell/weak_ref.hpp` |
| Source | (header-only; resolver in `src/scripting/script_globals.cpp`) |

## Syntax

```cpp
namespace fjell {

struct WeakRefData {
    uint32_t id{0};
    uint32_t generation{0};
};

template <typename T>
class WeakRef {
public:
    WeakRef() = default;

    explicit operator bool() const;

    [[nodiscard]] T* get()        const;
    T*               operator->() const;
    T&               operator*()  const;

    [[nodiscard]] uint32_t id()         const;
    [[nodiscard]] uint32_t generation() const;

    bool operator==(const WeakRef&) const = default;

    WeakRefData data_;
};

}
```

## Remarks

`WeakRef<T>` is for *scene-side things that might be destroyed and replaced under you* — other nodes, other components, spawned objects you want to track. Compare with [`Ref<T>`](Ref.md), which is for engine assets that live for the project session.

The handle stores `(id, generation)`. When a slot is reused (e.g. a node is destroyed and a new one takes its index), the generation bumps. A WeakRef whose stored generation doesn't match the current one resolves to `nullptr` — the classic "use-after-free → wrong object" bug doesn't happen.

`bool(ref)` is **not** equivalent to `id() != 0`. The bool conversion calls `get()` and checks for null, which detects stale generations. `id() != 0` only checks whether anything was ever assigned.

WeakRef costs the same as `Ref` per dereference (one indirect call into the engine). Cache the pointer if you dereference in a hot loop.

## Members

| Name | Type | Description |
|---|---|---|
| `data_` | `WeakRefData` | The (id, generation) pair. Public for engine access. |

## Functions

| Name | Description |
|---|---|
| [`operator bool`](#weakrefoperator-bool) | True if the ref currently resolves |
| [`get`](#weakrefget) | Resolve to `T*` (or null on stale/null) |
| [`operator->`](#weakrefoperator-arrow) | Resolve and dereference (no logging) |
| [`operator*`](#weakrefoperator-deref) | Resolve and dereference |
| [`id`](#weakrefid) | The stored ID |
| [`generation`](#weakrefgeneration) | The stored generation |
| `operator==` | Defaulted; compares both `id` and `generation` |

---

## WeakRef::operator bool

### Syntax
```cpp
explicit operator bool() const;
```

### Return value
`true` if `get()` would return non-null — both the ID is set *and* the generation matches the current engine state.

### Remarks
Use this in `if` conditions rather than `id() != 0`. It catches both unassigned and stale refs.

---

## WeakRef::get

### Syntax
```cpp
[[nodiscard]] T* get() const;
```

### Return value
Pointer to the referenced object, or `nullptr` if:
- the ref was never assigned (`id == 0`), or
- the slot was reused (stored `generation` != current `generation`), or
- the object was destroyed.

### Remarks
The right way to dereference. Always null-check.

```cpp
if (auto* t = target.get()) {
    t->take_damage(10);
}
```

---

## WeakRef::operator->  <a id="weakrefoperator-arrow"></a>

### Syntax
```cpp
T* operator->() const;
```

Calls `get()` and returns the pointer. **No null-check logging** (unlike `Ref::operator->`). If the ref is null, the caller crashes. Use `get()` for the safe path.

---

## WeakRef::operator*  <a id="weakrefoperator-deref"></a>

### Syntax
```cpp
T& operator*() const;
```

Dereferences `get()`. Same crash-on-null behavior as `operator->`.

---

## WeakRef::id

### Syntax
```cpp
[[nodiscard]] uint32_t id() const;
```

### Return value
The stored ID. Use `bool(ref)` or `get()` to actually test resolvability.

---

## WeakRef::generation

### Syntax
```cpp
[[nodiscard]] uint32_t generation() const;
```

### Return value
The stored generation. Useful for debugging stale-ref bugs (compare against the engine's current value for that ID).

## Example

```cpp
class EnemyAI : public fjell::ScriptComponent {
    FJELL_COMPONENT(EnemyAI)

    [[fj::property]] fjell::WeakRef<PlayerStats> target;

    void tick(float /*dt*/) override {
        auto* p = target.get();
        if (!p) {
            // Player died or scene reloaded — pick a new target
            target = find_nearest_player();
            return;
        }
        // ... chase / attack ...
    }
};
```

Comparing refs:

```cpp
if (current_target == primary_target) { /* ... */ }
```

## See also
- [`Ref`](Ref.md) — for engine assets
- [Overview — Reference types](../Overview.md#reference-types)
