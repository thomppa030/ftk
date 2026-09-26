# fjell::Delegate&lt;Signature&gt;

Multicast event with RAII subscription handles. Many listeners can subscribe; all fire on `broadcast()`.

| | |
|---|---|
| Header | `fjell/delegate.hpp` |
| Source | (header-only) |

## Syntax

```cpp
namespace fjell {

class Connection {
public:
    Connection() = default;
    ~Connection();   // disconnects

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;

    void disconnect();
    [[nodiscard]] bool connected() const;
};

template <typename... Args>
class Delegate<void(Args...)> {
public:
    Delegate();

    Delegate(const Delegate&) = delete;
    Delegate& operator=(const Delegate&) = delete;
    Delegate(Delegate&&) noexcept = default;
    Delegate& operator=(Delegate&&) noexcept = default;

    template <typename F>
    [[nodiscard]] Connection bind(F&& callback);

    void broadcast(Args... args);
    void clear();

    [[nodiscard]] bool     empty() const;
    [[nodiscard]] uint32_t size()  const;
};

}
```

## Remarks

A `Delegate<void(Args...)>` holds a list of listeners; calling `broadcast(args...)` invokes each with the same arguments. The signature must return `void` — there's no aggregation of return values.

Each `bind` returns a `Connection`. **Connections own the subscription**. Dropping a `Connection` (destructor, reassignment, move-out) automatically unsubscribes — the canonical safe pattern for event systems.

Re-entrant broadcasts are safe: if a listener calls `broadcast` recursively or disconnects another listener mid-broadcast, the engine defers list mutation until the outer `broadcast` completes (via `broadcast_depth` + `dirty` flags). Disconnecting your *own* connection from inside a callback is fine.

Delegates are non-copyable but movable. Internal state is a `shared_ptr`, so a `Connection` outliving its `Delegate` doesn't crash — it just becomes a no-op.

## Connection

The RAII subscription handle returned by `Delegate::bind`.

### Member functions

| Name | Description |
|---|---|
| Default constructor | Empty (disconnected) connection. |
| Destructor | Calls `disconnect()`. |
| Move constructor / assignment | Transfers the disconnect closure; the moved-from connection becomes empty. |
| [`disconnect`](#connectiondisconnect) | Manually unsubscribe. Idempotent. |
| [`connected`](#connectionconnected) | True if currently subscribed. |

Copy operations are deleted — connections are move-only.

---

### Connection::disconnect

```cpp
void disconnect();
```

Unsubscribes the listener. Safe to call multiple times — subsequent calls are no-ops. If the parent `Delegate` no longer exists, this is a no-op (the underlying state is gone).

---

### Connection::connected

```cpp
[[nodiscard]] bool connected() const;
```

`true` if the connection holds a live subscription. Doesn't verify the delegate still exists — only that the disconnect closure is set.

## Delegate functions

| Name | Description |
|---|---|
| [`bind`](#delegatebind) | Subscribe a callable; returns a `Connection` |
| [`broadcast`](#delegatebroadcast) | Fire all listeners |
| [`clear`](#delegateclear) | Remove all listeners |
| [`empty`](#delegateempty) | True if no listeners |
| [`size`](#delegatesize) | Listener count |

---

## Delegate::bind

### Syntax
```cpp
template <typename F>
    requires std::invocable<F, Args...>
[[nodiscard]] Connection bind(F&& callback);
```

### Parameters
| Name | Type | Description |
|---|---|---|
| `callback` | `F&&` | Anything invocable with `Args...` — lambda, `std::function`, function pointer, member function via lambda capture. |

### Return value
A `Connection`. **Must be stored** — if you ignore it, the destructor fires immediately and unsubscribes you. The `[[nodiscard]]` attribute will warn you.

### Remarks
The callback is stored in a `std::move_only_function`. Move-only callables work; copies aren't required.

### Example
```cpp
struct Listener {
    fjell::Connection death_conn_;

    void hook_up(PlayerStats& stats) {
        death_conn_ = stats.on_death.bind([this] {
            play_game_over();
        });
    }
};
```

Storing many in a vector:

```cpp
std::vector<fjell::Connection> conns_;
conns_.push_back(stats.on_health_changed.bind(/*...*/));
conns_.push_back(stats.on_mana_changed.bind(/*...*/));
```

---

## Delegate::broadcast

### Syntax
```cpp
void broadcast(Args... args);
```

### Parameters
| Name | Description |
|---|---|
| `args...` | Forwarded to every listener. |

### Remarks
Listeners fire in subscription order. Modifications to the listener list during broadcast (via `bind` / `disconnect`) are queued and applied after the outermost broadcast completes — listeners added mid-broadcast do not fire for the current event.

Exception thrown by a listener will propagate. The delegate's bookkeeping is exception-safe (depth/dirty restored).

---

## Delegate::clear

### Syntax
```cpp
void clear();
```

Removes all listeners. Outstanding `Connection`s become `connected() == false`.

If called during a broadcast, listeners are marked for removal but the current broadcast still processes them.

---

## Delegate::empty

### Syntax
```cpp
[[nodiscard]] bool empty() const;
```

`true` if no listeners.

---

## Delegate::size

### Syntax
```cpp
[[nodiscard]] uint32_t size() const;
```

Number of listeners currently subscribed.

## Patterns

### Publisher

```cpp
class PlayerStats : public fjell::ScriptComponent {
    FJELL_COMPONENT(PlayerStats)

    fjell::Delegate<void(int, int)> on_health_changed;   // (old, new)
    fjell::Delegate<void()>         on_death;

    void take_damage(int amount) {
        int old = hp_;
        hp_ = std::max(0, hp_ - amount);
        on_health_changed.broadcast(old, hp_);
        if (hp_ <= 0) on_death.broadcast();
    }

    int hp_{100};
};
```

### Subscriber

```cpp
class HurtFlash : public fjell::ScriptComponent {
    FJELL_COMPONENT(HurtFlash)

    fjell::Connection conn_;

    void begin_play() override {
        auto* stats = find_player_stats();
        conn_ = stats->on_health_changed.bind(
            [this](int old_hp, int hp) {
                if (hp < old_hp) flash_for_ = 0.2f;
            });
    }
    // No need to disconnect — `conn_`'s destructor handles it.

    float flash_for_ = 0.0f;
};
```

## See also
- [Overview — Events with Delegate](../Overview.md#events-with-delegate)
- [`Timer`](Timer.md) — for time-driven (rather than event-driven) callbacks
