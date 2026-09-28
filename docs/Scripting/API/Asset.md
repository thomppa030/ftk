# fjell::Asset&lt;T&gt;

A reference to an engine-owned asset: a preset, an audio clip, a VFX effect, an input
action, a surface, a material, weather.

| | |
|---|---|
| Header | `fjell/asset.hpp` |
| Source | (header-only; resolved and reported through `src/scripting/script_manager.cpp`) |

## Syntax

```cpp
namespace fjell {

template <typename T>
class Asset {
public:
    Asset() = default;

    explicit operator bool()           const;
    [[nodiscard]] uint32_t handle_id() const;
    bool operator==(const Asset&)      const;
};

}
```

`T` is a kind of asset: `Preset`, `Audio`, `Vfx`, `Surface`, `Weather`, `Climate`,
`DayProfile`, `Material` or `InputAction`.

## Remarks

Hold it as a `[[fj::property]]` and pick the file in the inspector, which offers the
assets of that kind and takes one dragged from the content browser. The scene file saves
the file.

It is **not** for things in the scene (other nodes, other components): use
[`Ref<T>`](Ref.md) for those.

**A script uses an asset by handing it to the engine**: `audio::play(clip, ...)`,
`scene::spawn(enemy, ...)`, `input::bind(jump, ...)`. None of the asset kinds has members
a script reads, so there is nothing to reach through the `Asset` itself.

**Handing over one with nothing there stops the game**, shipped builds included, as
using a gone [`Ref`](Ref.md) does: in the editor play ends with the script's node
selected, a functional test fails with the message, and a shipped game writes
`crash_report.txt` beside its executable and closes. The report says which script, which
hook, which field and why there was nothing:

```
Coin.on_trigger_enter on 'Coin' used `collect_sound` (an Asset<Audio>) with nothing there: it was never set
```

Nothing is there when the field was left empty, or when the file it names failed to load.
So leave a field empty only where the script checks it, with `if (asset)`:

```cpp
[[fj::property]] fjell::Asset<fjell::Preset> enemy;   // always set
[[fj::property]] fjell::Asset<fjell::Audio> fanfare;  // optional

void begin_play() override {
    fjell::scene::spawn(enemy, {0, 1, 0});
    if (fanfare) fjell::audio::play(fanfare, {0, 1, 0});
}
```

**A file that fails to load is kept.** The field goes on naming it: the inspector shows
it as missing, saving the scene writes it back rather than erasing it, and the report says
which file failed. The next time the scene loads, the file is tried again, and a field
holding it finds it once it loads.

**Which materials an `Asset<Material>` takes.** It takes any material unless it says which
shader types it can use:

```cpp
[[fj::shader_type(water)]] fjell::Asset<fjell::Material> calm_sea;
[[fj::shader_type(water, fog)]] fjell::Asset<fjell::Material> surface_or_medium;
```

The picker then lists only materials whose shader declares one of those types, a drop of
another type is refused with the reason shown over the field, and a field already holding
the wrong type says so under it. `fj::shader_type` implies `fj::property`. On anything
other than an `Asset<Material>` it is a build error; a name that is not a shader type
(`spatial`, `sky`, `ui`, `terrain`, `water`, `fog`) is reported when the engine loads the
module.

## Functions

| Name | Description |
|---|---|
| [`operator bool`](#assetoperator-bool) | Whether it holds an asset that is there |
| [`handle_id`](#assethandle_id) | The asset's handle, 0 while unset |
| [`operator==`](#assetoperator) | The same asset |

---

## Asset::operator bool

```cpp
explicit operator bool() const;
```

Whether it holds an asset that is there: false while the field is empty, and when the
file it names failed to load. Asks the engine each time; it is cheap.

---

## Asset::handle_id

```cpp
[[nodiscard]] uint32_t handle_id() const;
```

The asset's handle, 0 while unset. A file that failed to load has a handle of its own, so
this is not a check that the asset is there. Physics reports the surface a hit touched by
this handle; compare it with `surface::is` (`fjell/surface_api.hpp`).

---

## Asset::operator==

```cpp
bool operator==(const Asset& other) const;
```

The same asset: the same file, or both unset. How a script tells which weather is playing,
`fjell::weather::current() == storm`.

## See also

- [`Ref`](Ref.md) — references to components on nodes
- [Overview — Reference types](../Overview.md#reference-types)
