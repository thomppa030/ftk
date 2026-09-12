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

/// Plays sound effects.
struct Audio {
    /// Play a one-shot sound at a world position. The sound is positional —
    /// it attenuates with distance from the active camera — and finishes on
    /// its own, so it outlives the node that started it.
    /// @param clip   Audio asset, normally a `[[fj::property]] Ref<AudioAsset>`
    ///               field assigned in the inspector. Unassigned refs are
    ///               ignored rather than treated as an error.
    /// @param pos    World-space position to play from.
    /// @param volume Linear gain. 1.0 is the clip's authored level.
    /// @param pitch  Playback rate multiplier; also shifts pitch. 1.0 is
    ///               unchanged, 2.0 is an octave up and twice as fast.
    /// @note Fire-and-forget — there is no handle, so a one-shot cannot be
    ///       stopped or looped once started.
    /// @code
    /// [[fj::property]] fjell::Ref<fjell::AudioAsset> footstep;
    /// // ...
    /// fjell::Audio::play(footstep, transform().position, 0.8f);
    /// @endcode
    static void play(const Ref<AudioAsset>& clip, const glm::vec3& pos,
                     float volume = 1.0f, float pitch = 1.0f) {
        if (clip.handle_id() != 0) {
            fjell_audio_play(clip.handle_id(), pos.x, pos.y, pos.z,
                             volume, pitch);
        }
    }
};

} // namespace fjell
