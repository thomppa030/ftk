# fjell::Timer

Schedules one-shot and repeating callbacks on the main thread.

| | |
|---|---|
| Header | `fjell/timer_api.hpp` |
| Source | `src/scripting/timer_api.cpp` (impl), `src/scripting/timer_manager.cpp` (engine-side manager) |

## Syntax

```cpp
namespace fjell {

struct TimerHandle {
    uint64_t packed{0};
    [[nodiscard]] bool valid() const;
};

struct Timer {
    template <auto Method, typename T>
    static TimerHandle set(float delay, T* instance);

    template <auto Method, typename T>
    static TimerHandle set_repeating(float interval, T* instance);

    static void cancel(TimerHandle handle);
};

}
```

## Remarks

Timers run on the main thread between ticks. Their delays are measured in the **gameplay clock** — when [`Time::set_scale(0.5f)`](Time.md) is active, timer countdowns run at half real-time. When [`Time::pause()`](Time.md) is active, all timers are frozen.

Handles are `(index, generation)` packed into 64 bits. The generation bumps when a slot is recycled, so calling `cancel()` on an expired or already-cancelled handle is safe — the engine sees the stale generation and ignores it.

Timers are **not auto-cancelled** when your component is destroyed. If a timer fires after `end_play()` and calls a member function on a deleted `this`, you'll crash. Cancel in `end_play()` (or store handles in a `WeakRef`-style structure if you need to outlive the owner).

## Types

### TimerHandle

```cpp
struct TimerHandle {
    uint64_t packed{0};
    [[nodiscard]] bool valid() const;  // true if packed != 0
};
```

A returned handle. The `packed` field is the engine's internal `(index, generation)` representation; don't read or modify it directly. `valid()` distinguishes "never set" / "engine ran out of slots" from a real handle.

## Functions

| Name | Description |
|---|---|
| [`set`](#timerset) | Schedule a one-shot callback after `delay` |
| [`set_repeating`](#timerset_repeating) | Schedule a repeating callback every `interval` |
| [`cancel`](#timercancel) | Cancel a scheduled timer |

---

## Timer::set

### Syntax
```cpp
template <auto Method, typename T>
static TimerHandle set(float delay, T* instance);
```

### Template parameters
| Name | Description |
|---|---|
| `Method` | Pointer-to-member-function on `T` taking no arguments and returning `void`. |
| `T` | Type of `instance`. Deduced. |

### Parameters
| Name | Type | Description |
|---|---|---|
| `delay` | `float` | Seconds (gameplay time) until the callback fires. Values `≤ 0` fire on the next tick. |
| `instance` | `T*` | Object the method is called on. The pointer is captured — must remain valid until the callback fires (or until `cancel`). |

### Return value
A `TimerHandle`. Returns `{0}` (invalid) if the timer manager is not currently available — extremely rare; would indicate the engine is mid-shutdown.

### Remarks
The callback receives no arguments. To pass per-call data, capture in the member function's `this` state.

The internal lambda is `[fn, instance]() { fn(instance); }` — just one indirect call, no allocation per fire.

### Example
```cpp
class Spawner : public fjell::ScriptComponent {
    FJELL_COMPONENT(Spawner)

    fjell::TimerHandle next_;

    void begin_play() override {
        next_ = fjell::Timer::set<&Spawner::spawn_one>(2.0f, this);
    }
    void end_play() override { fjell::Timer::cancel(next_); }

    void spawn_one() {
        /* ... */
        next_ = fjell::Timer::set<&Spawner::spawn_one>(2.0f, this);
    }
};
```

---

## Timer::set_repeating

### Syntax
```cpp
template <auto Method, typename T>
static TimerHandle set_repeating(float interval, T* instance);
```

### Template parameters
| Name | Description |
|---|---|
| `Method` | Pointer-to-member-function on `T` taking no arguments and returning `void`. |
| `T` | Type of `instance`. Deduced. |

### Parameters
| Name | Type | Description |
|---|---|---|
| `interval` | `float` | Seconds between fires. The first fire occurs after `interval` seconds — *not* immediately on registration. |
| `instance` | `T*` | Object the method is called on. Captured by pointer; must outlive the timer. |

### Return value
A `TimerHandle`. Always non-zero on success.

### Remarks
Re-arms automatically after each fire. The phase is set by the registration time; intervals drift slightly if a fire is delayed (e.g. by a long frame) but the engine does not accumulate "missed" fires.

Cancel via `Timer::cancel(handle)` or by letting `end_play()` cancel it explicitly.

### Example
```cpp
void begin_play() override {
    pulse_ = fjell::Timer::set_repeating<&Heart::on_pulse>(1.0f, this);
}
void on_pulse() { fjell::Log::info("ba-thump"); }
```

---

## Timer::cancel

### Syntax
```cpp
static void cancel(TimerHandle handle);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `handle` | `TimerHandle` | Handle returned by `set` or `set_repeating`. Invalid handles are safely ignored. |

### Remarks
Safe to call multiple times on the same handle. Safe to call on a handle whose timer has already fired (one-shot) or whose slot has been reused. The generation field detects stale handles.

After `cancel`, the timer will never fire again. There is no "pause" — re-arm by calling `set` / `set_repeating` again.

## Example — full lifecycle

```cpp
class Rotator : public fjell::ScriptComponent {
    FJELL_COMPONENT(Rotator)

    fjell::TimerHandle timer_;

    void begin_play() override {
        timer_ = fjell::Timer::set_repeating<&Rotator::on_tick>(2.0f, this);
    }

    void end_play() override {
        fjell::Timer::cancel(timer_);
    }

    void on_tick() {
        fjell::Log::info("tick");
    }
};
```

## See also
- [`Time`](Time.md) — the clock that drives timer countdowns
- [`Delegate`](Delegate.md) — for event-driven (rather than time-driven) callbacks
