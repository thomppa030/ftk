#include "scripting/script_globals.hpp"

namespace fjell {

static Scene* s_active_scene = nullptr;
static ScriptManager* s_active_script_mgr = nullptr;
static ResourceManager* s_resource_manager = nullptr;
static const Project* s_active_project = nullptr;
static VfxSubsystem* s_vfx_subsystem = nullptr;
static AudioSubsystem* s_audio_subsystem = nullptr;
static CameraSubsystem* s_camera_subsystem = nullptr;
static VulkanContext* s_vulkan_context = nullptr;

Scene*& script_active_scene() { return s_active_scene; }
ScriptManager*& script_active_manager() { return s_active_script_mgr; }
ResourceManager*& script_resource_manager() { return s_resource_manager; }
const Project*& script_active_project() { return s_active_project; }
VfxSubsystem*& script_vfx_subsystem() { return s_vfx_subsystem; }
AudioSubsystem*& script_audio_subsystem() { return s_audio_subsystem; }
CameraSubsystem*& script_camera_subsystem() { return s_camera_subsystem; }
VulkanContext*& script_vulkan_context() { return s_vulkan_context; }

} // namespace fjell
