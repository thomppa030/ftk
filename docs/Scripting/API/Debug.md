# fjell::Debug

Submits primitives to the engine's debug-draw queue, and text to the on-screen debug overlay.

| | |
|---|---|
| Header | `fjell/debug_api.hpp` |
| Source | `src/scripting/debug_api.cpp` |

## Syntax

```cpp
namespace fjell {
struct Debug {
    static void line  (const glm::vec3& from, const glm::vec3& to,
                       const glm::vec4& color = {0,1,0,1}, float duration = 0.0f);
    static void box   (const glm::vec3& center, const glm::vec3& half_extents,
                       const glm::vec4& color = {0,1,0,1}, float duration = 0.0f);
    static void sphere(const glm::vec3& center, float radius,
                       const glm::vec4& color = {0,1,0,1}, float duration = 0.0f);
    static void point (const glm::vec3& pos,
                       const glm::vec4& color = {1,1,0,1}, float duration = 0.0f);
    static void ray   (const glm::vec3& origin, const glm::vec3& direction,
                       const glm::vec4& color = {0,1,0,1}, float duration = 0.0f);

    // On-screen text
    static void screen (int key, const char* text,
                        const glm::vec4& color = {1,1,1,1}, float duration = 0.0f);
    static void screen_f(int key, const glm::vec4& color, float duration,
                         const char* fmt, ...);
    static void print  (const char* text,
                        const glm::vec4& color = {1,1,1,1}, float duration = 2.0f);
    static void print_f(const glm::vec4& color, float duration,
                        const char* fmt, ...);
    static void watch  (const char* name, const char* value,
                        const glm::vec4& color = {1,1,1,1});
    static void watch_f(const char* name, const char* fmt, ...);
    static void watch_cf(const char* name, const glm::vec4& color,
                         const char* fmt, ...);
    static void text_3d(const glm::vec3& world_pos, const char* text,
                        const glm::vec4& color = {1,1,1,1}, float duration = 0.0f);
    static void text_3d_f(const glm::vec3& world_pos, const glm::vec4& color,
                          float duration, const char* fmt, ...);
};
}
```

## Remarks

Primitives are queued, then drawn by the renderer's debug pass at the end of the frame. They're rendered as wireframe with depth-test enabled — they're occluded by opaque geometry. `duration = 0.0f` means *single frame*; positive values persist across frames until the duration elapses.

Always present in builds — there is no debug-only conditional. Cost is low (a few floats per primitive into a CPU vector) but not free; large quantities every frame add up. The renderer batches them into a single draw via a dedicated debug pipeline.

The text calls (`screen`, `print`, `watch`, `text_3d`) go to a separate screen-space queue drawn by the ImGui overlay rather than the wireframe pass — see [On-screen text](#on-screen-text).

Colors are RGBA in `[0, 1]`. The default is opaque green for shape primitives, yellow for points, and white for text.

## Functions

| Name | Description |
|---|---|
| [`line`](#debugline) | Draw a line segment |
| [`box`](#debugbox) | Draw a wireframe AABB |
| [`sphere`](#debugsphere) | Draw a wireframe sphere |
| [`point`](#debugpoint) | Draw a small cross marker |
| [`ray`](#debugray) | Convenience: line from `origin` to `origin + direction` |
| [`screen`](#debugscreen) | Keyed on-screen message, replaced in place |
| [`screen_f`](#debugscreen_f) | Keyed message, printf-formatted |
| [`print`](#debugprint) | Unkeyed on-screen message, appends |
| [`print_f`](#debugprint_f) | Unkeyed message, printf-formatted |
| [`watch`](#debugwatch) | Named `name: value` row |
| [`watch_f`](#debugwatch_f) | Named row, printf-formatted |
| [`watch_cf`](#debugwatch_cf) | Named row, printf-formatted, explicit colour |
| [`text_3d`](#debugtext_3d) | Text anchored to a world position |
| [`text_3d_f`](#debugtext_3d_f) | World-anchored text, printf-formatted |

---

## Debug::line

### Syntax
```cpp
static void line(const glm::vec3& from, const glm::vec3& to,
                 const glm::vec4& color = {0,1,0,1},
                 float duration = 0.0f);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `from` | `glm::vec3` | Start point in world space. |
| `to` | `glm::vec3` | End point in world space. |
| `color` | `glm::vec4` | RGBA in `[0,1]`. Default opaque green. |
| `duration` | `float` | Seconds to persist. `0` = single frame. |

---

## Debug::box

### Syntax
```cpp
static void box(const glm::vec3& center, const glm::vec3& half_extents,
                const glm::vec4& color = {0,1,0,1},
                float duration = 0.0f);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `center` | `glm::vec3` | Center of the AABB. |
| `half_extents` | `glm::vec3` | Half-size on each axis. Total box size is `2 * half_extents`. |
| `color` | `glm::vec4` | RGBA. |
| `duration` | `float` | Seconds to persist. |

### Remarks
Axis-aligned. For oriented boxes, build with `line` segments yourself.

---

## Debug::sphere

### Syntax
```cpp
static void sphere(const glm::vec3& center, float radius,
                   const glm::vec4& color = {0,1,0,1},
                   float duration = 0.0f);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `center` | `glm::vec3` | Sphere center. |
| `radius` | `float` | Sphere radius. |
| `color` | `glm::vec4` | RGBA. |
| `duration` | `float` | Seconds to persist. |

### Remarks
Drawn as three orthogonal circles (XY, XZ, YZ). Tessellation is fixed in the engine; you can't change segment count from script.

---

## Debug::point

### Syntax
```cpp
static void point(const glm::vec3& pos,
                  const glm::vec4& color = {1,1,0,1},
                  float duration = 0.0f);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `pos` | `glm::vec3` | World-space position. |
| `color` | `glm::vec4` | RGBA. Default opaque yellow. |
| `duration` | `float` | Seconds to persist. |

### Remarks
Rendered as a small 3D cross. Size is fixed in world units.

---

## Debug::ray

### Syntax
```cpp
static void ray(const glm::vec3& origin, const glm::vec3& direction,
                const glm::vec4& color = {0,1,0,1},
                float duration = 0.0f);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `origin` | `glm::vec3` | Ray start. |
| `direction` | `glm::vec3` | Ray vector — used as-is, **not** normalized. Length determines the drawn endpoint (`origin + direction`). |
| `color` | `glm::vec4` | RGBA. |
| `duration` | `float` | Seconds to persist. |

### Remarks
Convenience wrapper around `line(origin, origin + direction, ...)`. Does not normalize — pass a scaled vector for a specific length.

---

# On-screen text

The text calls write to a separate queue drawn by the ImGui overlay, over the
scene image — in play mode (including a standalone game build, where the same
overlay runs) and over the editor viewport panel while scripts tick.

Text is laid out in three blocks:

- **Watches** — top-left, sorted by name, one `name: value` row each.
- **Messages** — below the watches, in submission order.
- **World text** — wherever the anchor point projects to.

Every text call has a `_f` variant that takes a printf format string. Formatting
happens on the caller's side and only the finished string crosses the engine
boundary, so there are no varargs in the ABI. The format string is checked
against its arguments at compile time — `watch_f("hp", "%d", 2.5f)` is a
warning, not a silent garbage read. One line is capped at 512 bytes and
truncates rather than overflowing; nothing here allocates on the caller's side.

Because varargs must come last, the formatted variants take `color` and
`duration` *before* the format string rather than as trailing defaults.

## Choosing a call

| You want | Use |
|---|---|
| Track a value every frame | `watch_f` — one stable row, sorted by name |
| A steady line you refresh every frame | `screen_f` with a fixed key |
| A one-off event you want to read afterwards | `print_f` with a duration |
| A label pinned to something in the world | `text_3d_f` |

Calling `print` every frame from `tick()` appends a new line each frame and
scrolls the block — that's what `screen` and `watch` exist to avoid.

## Lifetime

`duration = 0` means the entry lives exactly one frame, so a call from `tick()`
shows a value that updates every frame and disappears the moment the script
stops writing it. Positive durations persist and age out on the unscaled clock,
so `Time::set_scale(0)` doesn't freeze a timed message on screen.

Watches always behave as single-frame entries — re-submitting a name refreshes
it in place, and a watch you stop writing disappears on the next frame.

---

## Debug::screen

### Syntax
```cpp
static void screen(int key, const char* text,
                   const glm::vec4& color = {1,1,1,1},
                   float duration = 0.0f);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `key` | `int` | Identity of the line. Re-using a key replaces that line in place, keeping its position. Negative appends instead (what `print` does). |
| `text` | `const char*` | The line to show. |
| `color` | `glm::vec4` | RGBA in `[0,1]`. Default opaque white. |
| `duration` | `float` | Seconds to persist. `0` = single frame. |

### Remarks
Keys are yours to allocate — any distinct `int` per call site works. They only
need to be unique among the lines you want kept apart.

---

## Debug::screen_f

### Syntax
```cpp
static void screen_f(int key, const glm::vec4& color, float duration,
                     const char* fmt, ...);
```

### Remarks
`screen` with a printf format string. Colour and duration precede `fmt` because
C varargs must be last. The format is compile-time checked against its
arguments.

---

## Debug::print

### Syntax
```cpp
static void print(const char* text,
                  const glm::vec4& color = {1,1,1,1},
                  float duration = 2.0f);
```

### Remarks
Appends a new line per call — for one-off events, not per-frame values. The
default duration is 2 seconds so an event stays readable after it fires.

---

## Debug::print_f

### Syntax
```cpp
static void print_f(const glm::vec4& color, float duration,
                    const char* fmt, ...);
```

---

## Debug::watch

### Syntax
```cpp
static void watch(const char* name, const char* value,
                  const glm::vec4& color = {1,1,1,1});
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `name` | `const char*` | Label, and the identity of the row. Re-submitting updates in place. |
| `value` | `const char*` | Value shown after the label. |
| `color` | `glm::vec4` | RGBA. |

### Remarks
Rows are sorted by name and held in their own block, so a row keeps the same
spot every frame instead of moving as other output comes and goes. There's no
explicit duration — a watch lives as long as you keep writing it.

---

## Debug::watch_f

### Syntax
```cpp
static void watch_f(const char* name, const char* fmt, ...);
```

### Remarks
The common case: `Debug::watch_f("hp", "%d/%d", hp, max_hp)`. Use `watch_cf`
when you also want a colour.

---

## Debug::watch_cf

### Syntax
```cpp
static void watch_cf(const char* name, const glm::vec4& color,
                     const char* fmt, ...);
```

---

## Debug::text_3d

### Syntax
```cpp
static void text_3d(const glm::vec3& world_pos, const char* text,
                    const glm::vec4& color = {1,1,1,1},
                    float duration = 0.0f);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `world_pos` | `glm::vec3` | World-space anchor. Text is centred horizontally on it. |
| `text` | `const char*` | The label. |
| `color` | `glm::vec4` | RGBA. |
| `duration` | `float` | Seconds to persist. `0` = single frame. |

### Remarks
Projected through the camera that rendered the view — the scene `CameraNode`
while playing, the editor fly-camera otherwise. Hidden when the anchor is
behind the camera or off-screen. Unlike the 3D primitives, it is **not**
occluded by geometry: text anchored behind a wall still shows.

---

## Debug::text_3d_f

### Syntax
```cpp
static void text_3d_f(const glm::vec3& world_pos, const glm::vec4& color,
                      float duration, const char* fmt, ...);
```

## Example

```cpp
void tick(float dt) override {
    auto p = transform().position;

    // 3D primitives
    fjell::Debug::sphere(p, 1.0f, {0, 1, 0.5f, 1});
    fjell::Debug::ray(p, {0, 0, -3}, {1, 0.3f, 0, 1});

    // Values you want to watch every frame
    fjell::Debug::watch_f("pos", "%.1f, %.1f, %.1f", p.x, p.y, p.z);
    fjell::Debug::watch_f("hp", "%d/%d", hp_, max_hp_);
    fjell::Debug::watch_cf("state", state_color(), "%s", state_name());

    // A steady line, refreshed in place each frame
    fjell::Debug::screen_f(0, {0.6f, 0.9f, 1, 1}, 0.0f, "dt=%.4f", dt);

    // A label pinned above this object
    fjell::Debug::text_3d_f(p + glm::vec3{0, 1.5f, 0},
                            {1, 1, 0.4f, 1}, 0.0f, "%s", name());
}

void on_raycast_hit(const glm::vec3& hit) {
    fjell::Debug::point(hit, {1, 0, 0, 1}, /*duration*/ 2.0f);

    // A one-off event, readable for a couple of seconds after it fires
    fjell::Debug::print_f({1, 0.5f, 0, 1}, 2.0f,
                          "hit at %.2f, %.2f, %.2f", hit.x, hit.y, hit.z);
}
```

## See also
- [`Log`](Log.md) — text logging counterpart
