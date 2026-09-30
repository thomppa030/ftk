#include "hub/hub_ui.hpp"
#include "core/clock.hpp"
#include "core/log.hpp"
#include "gpu/command_list.hpp"
#include "gpu/device.hpp"
#include "gpu/frame.hpp"
#include "gpu/swapchain.hpp"
#include "renderer/gpu/window.hpp"
#include "ui/engine_imgui_files.hpp"
#include "ui/imgui_layer.hpp"

#include "platform/platform.hpp"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static constexpr int HUB_WIDTH = 480;
static constexpr int HUB_HEIGHT = 760;

static void launch_editor(const std::string& project_path, const std::string& create_name) {
    auto exe = fjell::platform::executable_path();
    auto editor_path = exe.parent_path() / ("fjell" + std::string{fjell::platform::EXE_EXT});

    std::vector<std::string> args{editor_path.string(), project_path};
    if (!create_name.empty()) {
        args.emplace_back("--create-project");
        args.push_back(create_name);
    }

    auto result = fjell::platform::exec_replace(args);
    // exec_replace only returns on failure
    FJELL_CORE_ERROR("Failed to launch editor: {}", result.error());
}

int main() {
    fjell::log::init();
    std::string pending_launch;
    std::string pending_create_name;

    try {
        fjell::Window window{"Fjell", HUB_WIDTH, HUB_HEIGHT};
        auto made_device = fjell::gpu::Device::create(window);
        if (!made_device) throw std::runtime_error(made_device.error());
        const std::unique_ptr<fjell::gpu::Device> gpu_device = std::move(*made_device);
        fjell::gpu::Device& device = *gpu_device;
        auto made = fjell::gpu::Swapchain::create(device, window);
        if (!made) throw std::runtime_error(made.error());
        const std::unique_ptr<fjell::gpu::Swapchain> swapchain = std::move(*made);

        auto imgui_layer = fjell::ImGuiLayer(window, device, swapchain->format(), fjell::engine_imgui_files());

        fjell::hub::HubUI hub_ui;
        hub_ui.set_device(&device);
        fjell::Clock clock;

        while (!window.should_close()) {
            window.poll_events();
            hub_ui.tick_browsers();

            clock.tick();
            hub_ui.update_splash(clock.delta_time());

            fjell::gpu::Frame& frame = device.begin_frame();
            const auto image = swapchain->acquire(frame);
            if (!image) {
                (void)device.end_frame(frame);
                continue;
            }

            fjell::gpu::CommandList& cmd = frame.commands(fjell::gpu::Queue::graphics);
            const fjell::gpu::Transition draw = image->to_draw();
            cmd.transition({&draw, 1});
            std::string launch;
            {
                auto target = cmd.render({.color = {{.view = image->texture,
                                                     .load = fjell::gpu::Load::clear,
                                                     .clear = fjell::gpu::Clear{0.012f, 0.012f, 0.015f, 1.0f}}}});
                imgui_layer.begin_frame();
                launch = hub_ui.draw(static_cast<float>(swapchain->width()),
                                     static_cast<float>(swapchain->height()));
                imgui_layer.end_frame();
                imgui_layer.render(target);
            }
            const fjell::gpu::Transition show = image->to_present();
            cmd.transition({&show, 1});
            frame.submit(cmd);
            if (auto shown = swapchain->present(frame, *image); !shown) {
                throw std::runtime_error(shown.error());
            }

            if (!launch.empty()) {
                pending_launch = launch;
                pending_create_name = hub_ui.launch_create_name();
                window.request_close();
            }
        }

        device.wait_idle();

    } catch (const std::exception& e) {
        FJELL_CORE_CRITICAL("Fatal: {}", e.what());
        fjell::log::shutdown();
        return EXIT_FAILURE;
    }

    fjell::log::shutdown();

    // exec into the editor after all cleanup — replaces this process,
    // so the shell waits for the editor naturally and logs go to the terminal.
    if (!pending_launch.empty()) {
        launch_editor(pending_launch, pending_create_name);
    }

    return EXIT_SUCCESS;
}
