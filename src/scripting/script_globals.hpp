// script_globals.hpp — Shared state for scripting API implementations
// Internal to the engine, not shipped to game projects.
#pragma once

namespace fjell {

class Scene;
class ScriptManager;
class ResourceManager;
class VfxSubsystem;
class AudioSubsystem;
class CameraSubsystem;
class Project;
class VulkanContext;

// Set by ScriptManager::prepare/update each frame
Scene*& script_active_scene();
ScriptManager*& script_active_manager();

// Set once via ScriptManager::set_*() at startup
ResourceManager*& script_resource_manager();
const Project*& script_active_project();
VfxSubsystem*& script_vfx_subsystem();
AudioSubsystem*& script_audio_subsystem();
CameraSubsystem*& script_camera_subsystem();
VulkanContext*& script_vulkan_context();

} // namespace fjell
