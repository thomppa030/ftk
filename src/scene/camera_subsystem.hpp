#pragma once

#include "core/subsystem.hpp"

#include <cstdint>

namespace fjell {

class Scene;
struct CameraNode;

class CameraSubsystem : public Subsystem {
public:
    [[nodiscard]] std::string_view name() const override { return "Camera"; }
    [[nodiscard]] TickPolicy tick_policy() const override { return TickPolicy::PLAY_ONLY; }

    void on_play_start(const TickContext& ctx) override;
    void on_play_stop(const TickContext& ctx) override;

    void set_active(uint32_t node_id);
    [[nodiscard]] uint32_t active_id() const { return active_id_; }
    [[nodiscard]] CameraNode* active_camera(Scene& scene);
    [[nodiscard]] const CameraNode* active_camera(const Scene& scene) const;

private:
    uint32_t active_id_{0};
};

} // namespace fjell
