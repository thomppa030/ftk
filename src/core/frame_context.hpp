#pragma once

#include <cstdint>

namespace ftk {
class ThreadPool;
} // namespace ftk

namespace fjell {

class Clock;
class Input;
class PhysicsWorld;
class Project;
class ResourceManager;
class Scene;

struct TickContext {
    Scene&         scene;
    const Input&   input;
    const Clock&   clock;
    float          dt;

    PhysicsWorld*    physics{nullptr};
    const Project*   project{nullptr};
    ResourceManager* resources{nullptr};
    /// The engine's worker threads, for a subsystem with work too slow for the
    /// tick it is asked in. Null where a context is put together without them.
    ftk::ThreadPool*      workers{nullptr};
};

/// What a subsystem sees inside one fixed step. It has no Clock and no frame
/// delta on purpose: the simulation advances by `dt` from `time`, and
/// anything it read from the frame would make it behave differently at a
/// different frame rate.
struct FixedTickContext {
    Scene&         scene;
    /// The step's length, the same for every step of a session.
    float          dt;
    /// Simulation time when this step starts.
    double         time;
    /// How many steps this session took before this one.
    std::uint64_t  index;
    /// How many of this frame's steps are left, this one included. For a
    /// target the frame set (a kinematic moved in tick, a pose animation
    /// sampled per frame) to be spread evenly over the frame's steps rather
    /// than reached on the first. Nothing else in a step may depend on it.
    int            frame_steps_left{1};

    PhysicsWorld*    physics{nullptr};
    const Project*   project{nullptr};
    ResourceManager* resources{nullptr};
    ftk::ThreadPool*      workers{nullptr};
};

} // namespace fjell
