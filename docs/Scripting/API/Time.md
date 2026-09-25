# fjell::Time

Controls the gameplay clock — scale and pause.

| | |
|---|---|
| Header | `fjell/time_api.hpp` |
| Source | `src/scripting/time_api.cpp` |

## Syntax

```cpp
namespace fjell {
struct Time {
    static void  set_scale(float scale);
    static float scale();

    static void  pause();
    static void  resume();
    static bool  is_paused();

    static float fixed_delta();
};
}
```

## Remarks

The gameplay clock drives `tick(dt)`, the simulation's fixed steps (and so `fixed_tick`, physics and [`Timer`](Timer.md) callbacks), VFX simulation, and animation. Scale and pause change how much game time each frame feeds the steps, never the step's length: at half speed the steps come half as often. Rendering, editor UI, and input polling continue regardless.

`set_scale(0.0f)` is allowed but has the same observable effect as `pause()` for most systems — prefer `pause()` for clarity. Negative scales are accepted but most systems don't define reverse-time behavior; results are implementation-defined.

The scale is multiplicative with the engine's own dt. The `dt` value handed to `tick()` is already scaled — script code never sees raw real time through `dt`.

## Functions

| Name | Description |
|---|---|
| [`set_scale`](#timeset_scale) | Set the time multiplier |
| [`scale`](#timescale) | Get the current multiplier |
| [`pause`](#timepause) | Pause gameplay |
| [`resume`](#timeresume) | Resume gameplay |
| [`is_paused`](#timeis_paused) | Query pause state |
| [`fixed_delta`](#timefixed_delta) | The simulation's step length |

---

## Time::set_scale

### Syntax
```cpp
static void set_scale(float scale);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `scale` | `float` | New multiplier. `1.0` = normal, `0.5` = half-speed, `2.0` = double-speed. |

### Remarks
Applies on the next frame. Does not affect ticks already in flight.

---

## Time::scale

### Syntax
```cpp
[[nodiscard]] static float scale();
```

### Return value
The current time scale.

---

## Time::pause

### Syntax
```cpp
static void pause();
```

### Remarks
Freezes gameplay-driven systems. Equivalent in observable behavior to `set_scale(0)` but stored as a distinct flag — `resume()` restores the prior `scale`.

---

## Time::resume

### Syntax
```cpp
static void resume();
```

### Remarks
Clears the pause flag. The prior `scale` value (set via `set_scale`) is restored.

---

## Time::is_paused

### Syntax
```cpp
[[nodiscard]] static bool is_paused();
```

### Return value
`true` if the pause flag is set.

---

## Time::fixed_delta

### Syntax
```cpp
[[nodiscard]] static float fixed_delta();
```

### Return value
The simulation's step length in seconds: what `fixed_tick()` receives as
`step.dt`, the same for every step of a session. 1/60 unless the project sets
another rate under Project Settings > Simulation.

---

## Example

```cpp
// Hit-stop on damage taken: a tenth of speed for a moment. Timers run on game
// time, so 0.05 s of it is about half a second of real time at a tenth.
fjell::Time::set_scale(0.1f);
fjell::Timer::set<&Game::end_hitstop>(0.05f, this);

// Pause menu
fjell::Time::pause();
// ...show menu...
fjell::Time::resume();
```

## See also
- [`Timer`](Timer.md) — scheduled callbacks (also driven by the gameplay clock)
- [`ScriptComponent`](ScriptComponent.md) — `delta_time()` returns the scaled dt
