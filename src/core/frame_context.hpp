#pragma once

#include <cstdint>

namespace fjell {

class Clock;
class Input;
class PhysicsWorld;
class Project;
class ResourceManager;
class ThreadPool;
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
    ThreadPool*      workers{nullptr};
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

    PhysicsWorld*    physics{nullptr};
    const Project*   project{nullptr};
    ResourceManager* resources{nullptr};
    ThreadPool*      workers{nullptr};
};

} // namespace fjell
