// Links fjell-gpu alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its own
// dependencies. Running it opens a window, brings a device up and makes a
// buffer, a texture with a view and a sampler through the GPU interface, which
// takes a GPU and a display: it is run by hand, not as a test.

#include "core/log.hpp"
#include "gpu/device.hpp"
#include "gpu/vulkan/native.hpp"
#include "renderer/gpu/gpu_core.hpp"
#include "renderer/gpu/window.hpp"

#include <cstring>

int main() {
    fjell::log::init({.level = spdlog::level::warn});
    bool ran = false;
    {
        fjell::Window window("fjell-link-gpu", 320, 240);
        fjell::GpuCore core(window);
        fjell::gpu::Device& device = core.gpu_device();

        auto buffer = device.create(fjell::gpu::BufferDesc{
            .size = 256,
            .use = fjell::gpu::BufferUse::storage,
            .memory = fjell::gpu::Memory::upload,
            .name = "link_buffer",
        });
        auto texture = device.create(fjell::gpu::TextureDesc{
            .format = fjell::gpu::Format::rgba8_unorm,
            .width = 64,
            .height = 64,
            .mips = 2,
            .use = fjell::gpu::TextureUse::sampled | fjell::gpu::TextureUse::storage,
            .initial = fjell::gpu::Clear{1.0f, 0.0f, 1.0f, 1.0f},
            .name = "link_texture",
        });
        const fjell::gpu::Sampler sampler = device.sampler({.address = fjell::gpu::Address::clamp});

        if (buffer && texture && sampler.valid()) {
            const auto bytes = device.mapped(*buffer);
            std::memset(bytes.data(), 0xAB, bytes.size());
            const VkImageView level = fjell::gpu::vulkan::native_view(device, fjell::gpu::mip(*texture, 1));
            const bool same_sampler =
                device.sampler({.address = fjell::gpu::Address::clamp}) == sampler;
            ran = bytes.size() == 256 && level != VK_NULL_HANDLE && same_sampler &&
                  device.info(*texture).mips == 2;
        }
        core.upload_context().wait_all();
        if (buffer) buffer->reset();
        if (texture) texture->reset();
        core.wait_idle();
    }
    fjell::log::shutdown();
    return ran ? 0 : 1;
}
