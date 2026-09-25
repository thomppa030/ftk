#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace fjell {

class VulkanContext;
struct FixedStepStats;

class StatsPanel {
public:
    void init(VulkanContext* ctx);
    void draw(float dt, const char* title = "Stats");

    /// The play session's fixed steps, for the Simulation section. Only a
    /// context that plays the game has one; without it the section is left
    /// out.
    void set_simulation(const FixedStepStats* stats) { simulation_ = stats; }

private:
    void draw_header();
    void draw_rendering_section();
    void draw_frame_graph_section();
    void draw_simulation_section();
    void add_simulation_frame();

    static constexpr int FRAME_TIME_HISTORY_SIZE = 120;

    // The fixed steps of the frames in one averaging period, added up: steps
    // come zero, one or two a frame, and one frame's count flickers.
    struct StepTotals {
        int frames{0};
        int steps{0};
        int most_steps{0};
        double seconds{0.0};
        std::vector<double> parts;
    };

    VulkanContext* ctx_{nullptr};
    const FixedStepStats* simulation_{nullptr};
    // The period being added up, and the last whole one, which is drawn.
    StepTotals step_totals_;
    StepTotals shown_step_totals_;

    float fps_{0.0f};
    float frame_time_{0.0f};
    int frame_count_{0};
    float fps_timer_{0.0f};
    std::array<float, FRAME_TIME_HISTORY_SIZE> frame_time_history_{};
    int frame_time_index_{0};
};

} // namespace fjell
