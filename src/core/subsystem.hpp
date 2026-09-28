#pragma once

#include <string_view>

namespace fjell {

struct TickContext;
struct FixedTickContext;

/// A self-contained engine system ticked once per frame, and, if it is part
/// of the simulation, once per fixed step as well. Engine places each
/// subsystem in a phase group, which decides both the order and whether it
/// runs only during play.
class Subsystem {
public:
    virtual ~Subsystem() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;

    // Engine calls run(), run_fixed() and the notify_ wrappers; subclasses
    // override tick(), fixed_tick() and the on_ virtuals.

    /// Tick once, inside a profiler zone named after the subsystem.
    void run(const TickContext& ctx);

    /// Run one fixed step, inside a profiler zone named after the subsystem.
    void run_fixed(const FixedTickContext& ctx);

    void notify_play_start(const TickContext& ctx) { playing_ = true; on_play_start(ctx); }
    void notify_play_stop(const TickContext& ctx)  { on_play_stop(ctx); playing_ = false; }

    /// The scene the subsystem ticks is a different one now: loaded from a
    /// file or made new. Not called when a play session puts back the scene
    /// it started from, which is the same scene returning.
    void notify_scene_replaced() { on_scene_replaced(); }

    [[nodiscard]] bool playing() const { return playing_; }

    Subsystem() = default;
    Subsystem(const Subsystem&) = delete;
    Subsystem& operator=(const Subsystem&) = delete;
    Subsystem(Subsystem&&) = delete;
    Subsystem& operator=(Subsystem&&) = delete;

protected:
    virtual void tick(const TickContext& /*ctx*/) {}
    /// What the subsystem does in each fixed step of play. Only the step's
    /// own time is in the context: see FixedTickContext.
    virtual void fixed_tick(const FixedTickContext& /*ctx*/) {}
    virtual void on_play_start(const TickContext& /*ctx*/) {}
    virtual void on_play_stop(const TickContext& /*ctx*/) {}
    /// Let go of what was kept about the scene before. It is gone: its nodes
    /// with it, and its node ids may name different nodes in the new one.
    virtual void on_scene_replaced() {}

private:
    bool playing_{false};
};

} // namespace fjell
