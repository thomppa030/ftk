#pragma once

#include "core/frame_context.hpp"
#include "core/subsystem.hpp"
#include "scripting/script_manager.hpp"

#include <string_view>

namespace fjell {

class ScriptSubsystem : public Subsystem {
public:
    [[nodiscard]] std::string_view name() const override { return "Scripts"; }

    void tick(const TickContext& ctx) override {
        manager_.update(ctx.scene, ctx.input, ctx.clock, ctx.physics);
    }

    void on_play_start(const TickContext& ctx) override {
        manager_.prepare(ctx.scene, ctx.input, ctx.clock, ctx.physics);
        manager_.deserialize_all(ctx.scene);
        manager_.set_playing(true);
        manager_.call_begin_play(ctx.scene);
    }

    void on_play_stop(const TickContext& ctx) override {
        manager_.call_end_play(ctx.scene);
        manager_.set_playing(false);
    }

    [[nodiscard]] ScriptManager& manager() { return manager_; }
    [[nodiscard]] const ScriptManager& manager() const { return manager_; }

private:
    ScriptManager manager_;
};

} // namespace fjell
