#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "audio/audio_engine.hpp"
#include "audio/audio_component.hpp"
#include "scene/scene.hpp"
#include "scene/camera_node.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"

#include <unordered_map>

namespace fjell {

struct AudioEngine::Impl {
    ma_engine engine{};
    bool initialized{false};

    struct SoundEntry {
        ma_sound sound{};
        bool active{false};
    };

    std::unordered_map<uint32_t, SoundEntry> sounds;
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {
    ma_engine_config config = ma_engine_config_init();
    config.listenerCount = 1;

    ma_result result = ma_engine_init(&config, &impl_->engine);
    if (result != MA_SUCCESS) {
        FJELL_CORE_ERROR("Failed to initialize audio engine: {}", static_cast<int>(result));
        return;
    }

    impl_->initialized = true;
    FJELL_CORE_INFO("Audio engine initialized");
}

AudioEngine::~AudioEngine() {
    if (!impl_) return;
    stop_all();
    if (impl_->initialized) {
        ma_engine_uninit(&impl_->engine);
    }
}

bool AudioEngine::is_initialized() const {
    return impl_ && impl_->initialized;
}

void AudioEngine::update(Scene& scene, const glm::vec3& listener_pos,
                         const glm::vec3& listener_forward, const glm::vec3& listener_up) {
    FJELL_PROFILE_SCOPE_N("audio_update");
    if (!is_initialized()) return;

    // Update listener
    ma_engine_listener_set_position(&impl_->engine, 0,
        listener_pos.x, listener_pos.y, listener_pos.z);
    ma_engine_listener_set_direction(&impl_->engine, 0,
        listener_forward.x, listener_forward.y, listener_forward.z);
    ma_engine_listener_set_world_up(&impl_->engine, 0,
        listener_up.x, listener_up.y, listener_up.z);

    // Update positions of active sounds
    for (auto& [node_id, entry] : impl_->sounds) {
        if (!entry.active) continue;
        auto* node = scene.find_spatial(node_id);
        if (!node) continue;
        glm::vec3 pos = glm::vec3(node->world_matrix_[3]);
        ma_sound_set_position(&entry.sound, pos.x, pos.y, pos.z);
    }
}

Result<> AudioEngine::play(uint32_t node_id, Scene& scene, const Project* project) {
    if (!is_initialized()) return make_error("Audio engine not initialized");

    auto* base_node = scene.find(node_id);
    auto* audio_ptr = base_node ? base_node->components.get<AudioSourceComponent>() : nullptr;
    if (!audio_ptr) return make_error("Audio source component not found on node " + std::to_string(node_id));
    auto* node = base_node->as_spatial();

    auto& audio = *audio_ptr;
    if (audio.clip_path.empty()) return make_error("Audio clip path is empty");

    // Stop existing sound for this node
    stop(node_id);

    auto& entry = impl_->sounds[node_id];

    std::string resolved = base_path::resolve_asset(audio.clip_path, project);
    ma_result result = ma_sound_init_from_file(
        &impl_->engine, resolved.c_str(),
        MA_SOUND_FLAG_DECODE, nullptr, nullptr, &entry.sound);

    if (result != MA_SUCCESS) {
        impl_->sounds.erase(node_id);
        return make_error("Failed to load audio clip: " + audio.clip_path + " (resolved: " + resolved + ")");
    }

    ma_sound_set_volume(&entry.sound, audio.volume);
    ma_sound_set_pitch(&entry.sound, audio.pitch);
    ma_sound_set_looping(&entry.sound, audio.loop ? MA_TRUE : MA_FALSE);

    if (audio.spatial && node) {
        ma_sound_set_spatialization_enabled(&entry.sound, MA_TRUE);
        ma_sound_set_min_distance(&entry.sound, audio.min_distance);
        ma_sound_set_max_distance(&entry.sound, audio.max_distance);
        glm::vec3 pos = glm::vec3(node->world_matrix_[3]);
        ma_sound_set_position(&entry.sound, pos.x, pos.y, pos.z);
    } else {
        ma_sound_set_spatialization_enabled(&entry.sound, MA_FALSE);
    }

    ma_sound_start(&entry.sound);
    entry.active = true;
    audio.playing = true;
    return {};
}

void AudioEngine::stop(uint32_t node_id) {
    auto it = impl_->sounds.find(node_id);
    if (it == impl_->sounds.end()) return;
    ma_sound_uninit(&it->second.sound);
    impl_->sounds.erase(it);
}

void AudioEngine::stop_all() {
    for (auto& [id, entry] : impl_->sounds) {
        ma_sound_uninit(&entry.sound);
    }
    impl_->sounds.clear();
}

void AudioEngine::start_play(Scene& scene, const Project* project) {
    for (auto& obj : scene.objects()) {
        auto* au = obj->components.get<AudioSourceComponent>();
        if (au && au->play_on_start) {
            if (auto r = play(obj->id, scene, project); !r) {
                FJELL_CORE_WARN("Audio play failed for '{}': {}", obj->name, r.error());
            }
        }
    }
}

void AudioEngine::stop_play() {
    stop_all();
}

} // namespace fjell
