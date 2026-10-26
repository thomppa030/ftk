// Two windows drawn in one frame: the main one acquired as a program's own
// window, which waits for its image, the other only when it has one ready.
// Each second it prints how fast the main window draws, how many of those
// frames drew the other window, and the longest a frame and its end (where
// the images are presented) took. Hide the other window, move it to another
// workspace or a slower screen, or minimise it, and watch the main window's
// numbers.
//
//   ftk-probe-windows [seconds]

#include "ftk/base/log.hpp"
#include "ftk/gpu/command_list.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/frame.hpp"
#include "ftk/gpu/render_encoder.hpp"
#include "ftk/gpu/swapchain.hpp"
#include "ftk/gpu/window.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>

namespace {

using Clock = std::chrono::steady_clock;

double ms_between(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

// Fills `image` with a colour as a colour attachment, the stage its acquire
// is waited for at.
void fill(ftk::gpu::CommandList& cmd, const ftk::gpu::SwapchainImage& image, float shade) {
    const ftk::gpu::Transition to_draw = image.to_draw();
    cmd.transition({&to_draw, 1});
    {
        ftk::gpu::RenderTargets targets;
        targets.color.push_back({.view = image.texture,
                                 .load = ftk::gpu::Load::clear,
                                 .clear = ftk::gpu::Clear(shade, 0.25f, 1.0f - shade, 1.0f)});
        const ftk::gpu::RenderEncoder pass = cmd.render(targets);
    }
    const ftk::gpu::Transition to_present = image.to_present();
    cmd.transition({&to_present, 1});
}

// What one second of frames came to.
struct Second {
    int frames{0};
    int other_drawn{0};
    double frame_max{0.0};
    double end_max{0.0};
};

} // namespace

int main(int argc, char** argv) {
    ftk::log::init();
    const double seconds = argc > 1 ? std::atof(argv[1]) : 60.0;
    try {
        ftk::Window main_window("ftk probe: main window", 640, 400);
        auto device = ftk::gpu::Device::create(main_window);
        if (!device) {
            std::fprintf(stderr, "No GPU device: %s\n", device.error().c_str());
            return 1;
        }
        auto main_chain = ftk::gpu::Swapchain::create(**device, main_window);
        if (!main_chain) {
            std::fprintf(stderr, "No swapchain for the main window: %s\n", main_chain.error().c_str());
            return 1;
        }
        auto other_window = std::make_unique<ftk::Window>("ftk probe: other window", 480, 320);
        auto made = ftk::gpu::Swapchain::create(**device, *other_window);
        if (!made) {
            std::fprintf(stderr, "No swapchain for the other window: %s\n", made.error().c_str());
            return 1;
        }
        std::unique_ptr<ftk::gpu::Swapchain> other_chain = std::move(*made);

        const Clock::time_point start = Clock::now();
        Clock::time_point second_start = start;
        Second second;
        int printed = 0;
        while (ms_between(start, Clock::now()) < seconds * 1000.0) {
            main_window.poll_events();
            if (main_window.should_close()) break;
            if (other_window && other_window->should_close()) {
                other_chain.reset();
                other_window.reset();
            }

            const Clock::time_point frame_start = Clock::now();
            const float shade = 0.5f + 0.5f * std::sin(static_cast<float>(ms_between(start, frame_start)) * 0.002f);
            ftk::gpu::Frame& frame = (*device)->begin_frame();
            const auto main_image = (*main_chain)->acquire(frame);
            std::optional<ftk::gpu::SwapchainImage> other_image;
            if (other_chain) other_image = other_chain->acquire_ready(frame);
            ftk::gpu::CommandList& cmd = frame.commands(ftk::gpu::Queue::graphics);
            if (main_image) fill(cmd, *main_image, shade);
            if (other_image) fill(cmd, *other_image, 1.0f - shade);
            frame.submit(cmd);
            const Clock::time_point end_start = Clock::now();
            if (auto ended = (*device)->end_frame(frame); !ended) {
                std::fprintf(stderr, "The frame failed: %s\n", ended.error().c_str());
                break;
            }
            const Clock::time_point frame_end = Clock::now();

            ++second.frames;
            if (other_image) ++second.other_drawn;
            second.frame_max = std::max(second.frame_max, ms_between(frame_start, frame_end));
            second.end_max = std::max(second.end_max, ms_between(end_start, frame_end));
            if (ms_between(second_start, frame_end) >= 1000.0) {
                std::printf("%3ds  main %6.1f fps, longest frame %6.2f ms, longest end %6.2f ms;  other drawn in %d "
                            "of %d frames%s\n",
                            ++printed, second.frames * 1000.0 / ms_between(second_start, frame_end), second.frame_max,
                            second.end_max, second.other_drawn, second.frames, other_window ? "" : " (closed)");
                std::fflush(stdout);
                second = {};
                second_start = frame_end;
            }
        }
        (*device)->wait_idle();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}
