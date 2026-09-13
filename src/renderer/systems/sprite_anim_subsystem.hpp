#pragma once

#include "core/subsystem.hpp"

#include <string_view>

namespace fjell {

class ResourceManager;

class SpriteAnimSubsystem : public Subsystem {
public:
    [[nodiscard]] std::string_view name() const override { return "SpriteAnim"; }

    void set_resource_manager(ResourceManager* rm) { resources_ = rm; }

    void tick(const TickContext& ctx) override;
    void on_play_start(const TickContext& ctx) override;

private:
    ResourceManager* resources_{nullptr};
};

} // namespace fjell
