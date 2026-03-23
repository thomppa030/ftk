#pragma once

#include "core/result.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace fjell {

class Project;
class Scene;

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    AudioEngine(AudioEngine&&) = delete;
    AudioEngine& operator=(AudioEngine&&) = delete;

    // Update listener + all spatial sources from scene
    void update(Scene& scene, const glm::vec3& listener_pos,
                const glm::vec3& listener_forward, const glm::vec3& listener_up);

    // Play/stop individual sources
    Result<> play(uint32_t node_id, Scene& scene, const Project* project = nullptr);
    void stop(uint32_t node_id);
    void stop_all();

    // Fire-and-forget spatial sound (not tied to any node)
    void play_oneshot(const std::string& clip_path, const glm::vec3& position,
                      float volume = 1.0f, float pitch = 1.0f,
                      const Project* project = nullptr);

    // Trigger play_on_start sources (called when entering play mode)
    void start_play(Scene& scene, const Project* project = nullptr);

    // Stop everything (called when exiting play mode)
    void stop_play();

    [[nodiscard]] bool is_initialized() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fjell
