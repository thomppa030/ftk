#pragma once

#include "animation/animation_system.hpp"
#include "core/frame_context.hpp"
#include "core/subsystem.hpp"

#include <string_view>

namespace fjell {

class AnimationSubsystem : public Subsystem {
public:
    AnimationSubsystem(VkDevice device, VkPhysicalDevice physical_device)
        : system_{device, physical_device} {}

    [[nodiscard]] std::string_view name() const override { return "Animation"; }

    void tick(const TickContext& ctx) override {
        system_.update(ctx.scene, ctx.dt);
    }

    [[nodiscard]] AnimationSystem& system() { return system_; }
    [[nodiscard]] const AnimationSystem& system() const { return system_; }

private:
    AnimationSystem system_;
};

} // namespace fjell
