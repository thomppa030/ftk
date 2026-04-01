#include "scripting/script_globals.hpp"
#include "audio/audio_subsystem.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/resource_handle.hpp"

extern "C" void fjell_audio_play(uint32_t handle_id, float x, float y, float z,
                                 float volume, float pitch) {
    auto* audio = fjell::script_audio_subsystem();
    auto* rm = fjell::script_resource_manager();
    if (audio && rm) {
        audio->play_oneshot(fjell::AudioAssetHandle{handle_id},
                            {x, y, z}, *rm, volume, pitch,
                            fjell::script_active_project());
    }
}
