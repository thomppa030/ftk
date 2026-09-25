// script_globals.hpp — Shared state for scripting API implementations
// Internal to the engine, not shipped to game projects.
#pragma once

namespace fjell {

class AudioSubsystem;
class CameraSubsystem;
class Clock;
class DebugDrawQueue;
class EnvironmentSubsystem;
class FixedTimestep;
class Input;
class InputActionMap;
class NetManager;
class PhysicsWorld;
class Project;
class ResourceManager;
class Scene;
class ScreenDebugQueue;
class ScriptManager;
class StepInputLatch;
class TimerManager;
class VfxSubsystem;
class VulkanContext;

struct ScriptServices {
    Input* input{};
    ResourceManager* resources{};
    const Project* project{};
    VfxSubsystem* vfx{};
    AudioSubsystem* audio{};
    CameraSubsystem* camera{};
    EnvironmentSubsystem* environment{};
    VulkanContext* vulkan{};
    InputActionMap* input_actions{};
    TimerManager* timers{};
    Clock* clock{};
    DebugDrawQueue* debug_queue{};
    ScreenDebugQueue* screen_debug_queue{};
    NetManager* net{};

    // The simulation's clock and the button edges each of its steps sees.
    const FixedTimestep* fixed_timestep{};
    const StepInputLatch* step_input{};
    // Per-play: the physics world only exists while play mode runs.
    PhysicsWorld* physics{};
    // Set while scripts run a fixed step, the one place step input is valid.
    bool in_fixed_step{false};

    // Per-frame (set by ScriptManager::prepare/update)
    Scene* scene{};
    ScriptManager* manager{};
};

ScriptServices& script_services();

} // namespace fjell
