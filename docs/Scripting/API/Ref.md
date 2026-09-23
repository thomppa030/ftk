# fjell::Ref&lt;T&gt;

Handle to an engine-owned asset. Resolved through the engine on every dereference.

| | |
|---|---|
| Header | `fjell/ref.hpp` |
| Source | (header-only; resolver in `src/scripting/script_globals.cpp`) |

## Syntax

```cpp
namespace fjell {

struct RefData {
    uint32_t handle_id{0};
    uint8_t  asset_type_id{0};
    uint8_t  reserved_[3]{};
};

template <typename T>
class Ref {
public:
    Ref() = default;

    [[nodiscard]] bool valid() const;
    explicit operator bool() const;

    [[nodiscard]] T* get()       const;
    T*               operator->() const;
    T&               operator*()  const;

    [[nodiscard]] uint32_t handle_id() const;

    RefData data_;
};

}
```

## Remarks

`Ref<T>` references an *engine asset* — `VfxAsset`, `AudioAsset`, `PresetAsset`, `InputAction`, etc. The handle is 64 bits (`RefData`), stable across reloads, and serializable to `.fjscene`.

It is **not** for scene-side objects (other nodes, other components) — use [`WeakRef<T>`](WeakRef.md) for those. The difference matters because engine assets live in a different pool with different lifetime rules.

Assigned in the editor via the inspector's asset picker. When you declare:

```cpp
[[fj::property]] fjell::Ref<fjell::AudioAsset> collect_sound;
```

the inspector renders a drop-target for `AudioAsset` files. The `asset_type_id` byte in `RefData` is set when the inspector or deserializer writes the ref, and it gates `get()` — passing the wrong type returns null.

A `Ref<Material>` accepts any material by default. When the script can only use materials of certain shader types, say so:

```cpp
[[fj::shader_type(water)]] fjell::Ref<fjell::Material> calm_sea;
[[fj::shader_type(water, fog)]] fjell::Ref<fjell::Material> surface_or_medium;
```

The picker then lists only materials whose shader declares one of those types, a drop of another type is refused with the reason shown over the field, and a field already holding the wrong type says so under it. `fj::shader_type` implies `fj::property`. On anything other than a `Ref<Material>` it is a build error; a name that is not a shader type (`spatial`, `sky`, `ui`, `terrain`, `water`, `fog`) is reported when the engine loads the module.

Every dereference goes through `fjell_ref_resolve` (a C function exported by the engine). It's a pool lookup — cheap, but not free. If you dereference in a hot loop, cache the pointer at the top.

## Members

| Name | Type | Description |
|---|---|---|
| `data_` | `RefData` | The (handle, type) pair. Public for engine access — treat as opaque from game code. |

## Functions

| Name | Description |
|---|---|
| [`valid`](#refvalid) | True if the ref points at anything |
| [`operator bool`](#refoperator-bool) | Same as `valid()` |
| [`get`](#refget) | Resolve to `T*` (or null) |
| [`operator->`](#refoperator-arrow) | Resolve and dereference; logs on null |
| [`operator*`](#refoperator-deref) | Resolve and dereference; logs on null |
| [`handle_id`](#refhandle_id) | Raw 32-bit handle ID |

---

## Ref::valid

### Syntax
```cpp
[[nodiscard]] bool valid() const;
```

### Return value
`true` if `data_.handle_id != 0` — i.e., something was assigned in the inspector / deserialization.

### Remarks
A cheap structural check. Does **not** confirm the asset still exists in the engine — it only confirms the ref has been assigned. For most uses this is sufficient because engine assets are typically loaded for the life of the project session.

---

## Ref::operator bool

### Syntax
```cpp
explicit operator bool() const;
```

Same as `valid()`. Enables `if (ref) { ... }`.

---

## Ref::get

### Syntax
```cpp
[[nodiscard]] T* get() const;
```

### Return value
Pointer to the asset, or `nullptr` if the ref is unassigned or the engine can't resolve it.

### Remarks
The preferred way to dereference when you want to handle null gracefully:

```cpp
if (auto* clip = sound.get()) {
    // ... use clip ...
}
```

---

## Ref::operator->  <a id="refoperator-arrow"></a>

### Syntax
```cpp
T* operator->() const;
```

### Return value
Pointer to the asset.

### Remarks
**Logs an error if the ref is null** (via `fjell_log_error`) and returns null, which then crashes the caller. Use `get()` if you need to handle missing refs without log spam.

---

## Ref::operator*  <a id="refoperator-deref"></a>

### Syntax
```cpp
T& operator*() const;
```

### Return value
Reference to the asset.

### Remarks
Same null-logging behavior as `operator->`. Crashes if the ref is null.

---

## Ref::handle_id

### Syntax
```cpp
[[nodiscard]] uint32_t handle_id() const;
```

### Return value
The 32-bit handle ID. Used by some `*_api.hpp` functions that take handle IDs directly (`Audio::play`, `Vfx::spawn` — though those wrap it internally).

## Example

```cpp
class Coin : public fjell::ScriptComponent {
    FJELL_COMPONENT(Coin)

    [[fj::property]] fjell::Ref<fjell::AudioAsset> collect_sound;
    [[fj::property]] fjell::Ref<fjell::VfxAsset>   collect_effect;

    void on_trigger_enter(uint32_t /*other*/) override {
        // Silently no-op if refs unassigned
        fjell::Audio::play(collect_sound, transform().position);
        fjell::Vfx::spawn(collect_effect, transform().position);
        self().destroy();
    }
};
```

Validating in `begin_play()`:

```cpp
void begin_play() override {
    if (!collect_sound.valid()) {
        fjell::Log::warn("Coin: collect_sound not assigned");
    }
}
```

## See also
- [`WeakRef`](WeakRef.md) — for scene-side (generation-checked) references
- [Overview — Reference types](../Overview.md#reference-types)
