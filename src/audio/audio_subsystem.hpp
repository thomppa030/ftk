#pragma once

#include "audio/audio_engine.hpp"
#include "core/frame_context.hpp"
#include "core/subsystem.hpp"
#include "renderer/resource_handle.hpp"

#include <glm/glm.hpp>

#include <string_view>

namespace fjell {

class ResourceManager;

class AudioSubsystem : public Subsystem {
public:
    [[nodiscard]] std::string_view name() const override { return "Audio"; }

    void tick(const TickContext& ctx) override;

    void on_play_start(const TickContext& ctx) override {
        engine_.start_play(ctx.scene, ctx.project);
    }

    void on_play_stop(const TickContext& /*ctx*/) override {
        engine_.stop_play();
    }

    void play_oneshot(AudioAssetHandle asset, const glm::vec3& position,
                      ResourceManager& resources,
                      float volume_scale = 1.0f, float pitch_scale = 1.0f,
                      const Project* project = nullptr);

    [[nodiscard]] AudioEngine& engine() { return engine_; }
    [[nodiscard]] const AudioEngine& engine() const { return engine_; }

private:
    AudioEngine engine_;
};

} // namespace fjell
