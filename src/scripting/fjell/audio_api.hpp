// audio_api.hpp — Static audio API for game scripts
#pragma once

#include "fjell/api.h"
#include "fjell/ref.hpp"

#include <glm/glm.hpp>

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT void fjell_audio_play(uint32_t handle_id, float x, float y, float z,
                      float volume, float pitch);
}

namespace fjell {

struct AudioAsset;

struct Audio {
    static void play(const Ref<AudioAsset>& clip, const glm::vec3& pos,
                     float volume = 1.0f, float pitch = 1.0f) {
        if (clip.handle_id() != 0) {
            fjell_audio_play(clip.handle_id(), pos.x, pos.y, pos.z,
                             volume, pitch);
        }
    }
};

} // namespace fjell
