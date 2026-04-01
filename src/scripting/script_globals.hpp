// script_globals.hpp — Shared state for scripting API implementations
// Internal to the engine, not shipped to game projects.
#pragma once

namespace fjell {

class AudioSubsystem;
class CameraSubsystem;
class Input;
class InputActionMap;
class Project;
class ResourceManager;
class Scene;
class ScriptManager;
class VfxSubsystem;
class VulkanContext;

struct ScriptServices {
    Input* input{};
    ResourceManager* resources{};
    const Project* project{};
    VfxSubsystem* vfx{};
    AudioSubsystem* audio{};
    CameraSubsystem* camera{};
    VulkanContext* vulkan{};
    InputActionMap* input_actions{};

    // Per-frame (set by ScriptManager::prepare/update)
    Scene* scene{};
    ScriptManager* manager{};
};

ScriptServices& script_services();

} // namespace fjell
