#pragma once

#include <array>
#include <cstdint>

namespace fjell {

class VulkanContext;

class StatsPanel {
public:
    void init(VulkanContext* ctx);
    void draw(float dt, const char* title = "Stats");

private:
    static constexpr int FRAME_TIME_HISTORY_SIZE = 120;

    VulkanContext* ctx_{nullptr};

    float fps_{0.0f};
    float frame_time_{0.0f};
    int frame_count_{0};
    float fps_timer_{0.0f};
    std::array<float, FRAME_TIME_HISTORY_SIZE> frame_time_history_{};
    int frame_time_index_{0};
};

} // namespace fjell
