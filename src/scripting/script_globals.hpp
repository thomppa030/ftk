// script_globals.hpp — Shared state for scripting API implementations
// Internal to the engine, not shipped to game projects.
#pragma once

#include <cstdint>
#include <string_view>

namespace fjell {

struct AssetData;

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

/// What a script's `Asset` holds for the asset with handle `handle_id`, of
/// the kind scripts write `type_name` for (`"Weather"`): unset for 0.
[[nodiscard]] AssetData script_asset(std::string_view type_name, uint32_t handle_id);

/// The component type id scripts know by `type_name`, or
/// FJELL_NO_COMPONENT_TYPE: a script type of the loaded game library, else an
/// engine component the library has no script of that name for.
[[nodiscard]] uint32_t component_id_by_script_name(std::string_view type_name);

/// What a Ref finds in `scene`: the component of `type_id` on the node in its
/// `generation`th life, or for FJELL_NODE_TYPE the node itself when it is a
/// 3D node. Null when there is none.
[[nodiscard]] void* resolve_ref(Scene& scene, uint32_t node_id, uint32_t generation, uint32_t type_id);

} // namespace fjell
