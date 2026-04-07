#include "fjell/api.h"
#include "scripting/script_globals.hpp"
#include "audio/audio_subsystem.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/resource_handle.hpp"

extern "C" FJELL_SDK_EXPORT void fjell_audio_play(uint32_t handle_id, float x, float y, float z,
                                 float volume, float pitch) {
    auto& s = fjell::script_services();
    if (s.audio && s.resources) {
        s.audio->play_oneshot(fjell::AudioAssetHandle{handle_id},
                            {x, y, z}, *s.resources, volume, pitch,
                            s.project);
    }
}
