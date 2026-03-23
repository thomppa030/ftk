#pragma once

namespace fjell {

class AudioEngine;
class Clock;
class Input;
class PhysicsWorld;
class Project;
class Scene;
class ThreadPool;
class VfxSubsystem;

struct TickContext {
    Scene&         scene;
    const Input&   input;
    const Clock&   clock;
    float          dt;
    ThreadPool&    thread_pool;

    PhysicsWorld*  physics{nullptr};
    AudioEngine*   audio{nullptr};
    VfxSubsystem*  vfx{nullptr};
    const Project* project{nullptr};
};

} // namespace fjell
