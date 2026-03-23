#pragma once

#include "audio/audio_engine.hpp"
#include "core/frame_context.hpp"
#include "core/subsystem.hpp"

#include <string_view>

namespace fjell {

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

    [[nodiscard]] AudioEngine& engine() { return engine_; }
    [[nodiscard]] const AudioEngine& engine() const { return engine_; }

private:
    AudioEngine engine_;
};

} // namespace fjell
