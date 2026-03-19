#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace fjell {

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
    void play(uint32_t node_id, Scene& scene);
    void stop(uint32_t node_id);
    void stop_all();

    // Trigger play_on_start sources (called when entering play mode)
    void start_play(Scene& scene);

    // Stop everything (called when exiting play mode)
    void stop_play();

    [[nodiscard]] bool is_initialized() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fjell
