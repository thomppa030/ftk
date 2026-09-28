// Links fjell-gpu alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its own
// dependencies. Running it opens a window, brings a device up and makes a
// buffer, a texture with a view, a sampler and pipelines through the GPU
// interface, which takes a GPU and a display: it is run by hand, not as a
// test.

#include "core/log.hpp"
#include "gpu/device.hpp"
#include "gpu/vulkan/native.hpp"
#include "renderer/gpu/gpu_core.hpp"
#include "renderer/gpu/window.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

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

        // Pipelines from the test shaders the build compiled: compute, vertex
        // and mesh, one rebuilt in place, and one refused.
        device.set_shader_locator([](const std::string& relative) {
            return std::string(FJELL_TEST_SHADER_DIR "/") + relative;
        });
        auto compute = device.create(fjell::gpu::ComputePipelineDesc{.shader = "pipeline.comp"});
        auto graphics = device.create(fjell::gpu::GraphicsPipelineDesc{
            .vertex = "reflect.vert",
            .fragment = "reflect.frag",
            .color = {{fjell::gpu::Format::rgba8_unorm, fjell::gpu::Blend::alpha}},
            .name = "link_graphics",
        });
        auto mesh = device.create(fjell::gpu::GraphicsPipelineDesc{
            .mesh = "reflect.mesh",
            .depth = {.test = true, .write = true},
            .depth_format = fjell::gpu::Format::d32_float,
            .name = "link_mesh",
        });
        auto unbounded = device.create(fjell::gpu::ComputePipelineDesc{.shader = "reflect.comp"});
        bool pipelines = compute && graphics && mesh && !unbounded;
        if (!compute) std::fprintf(stderr, "%s\n", compute.error().c_str());
        if (!graphics) std::fprintf(stderr, "%s\n", graphics.error().c_str());
        if (!mesh) std::fprintf(stderr, "%s\n", mesh.error().c_str());
        // A shared layout for reflect.comp's set 4 (a table sized at run time
        // and a pair), which lets that pipeline be made once it names it; a
        // persistent group for pipeline.comp's own set, filled and refilled.
        std::array<VkDescriptorSetLayoutBinding, 2> table_bindings{};
        table_bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        table_bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo table_info{};
        table_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        table_info.bindingCount = static_cast<uint32_t>(table_bindings.size());
        table_info.pBindings = table_bindings.data();
        VkDescriptorSetLayout table_layout{VK_NULL_HANDLE};
        vkCreateDescriptorSetLayout(core.vk_device(), &table_info, nullptr, &table_layout);
        const fjell::gpu::SharedLayout table =
            fjell::gpu::vulkan::share_layout(device, "table", 4, table_layout, table_bindings);
        auto with_table = device.create(fjell::gpu::ComputePipelineDesc{
            .shader = "reflect.comp", .shared = {table}, .name = "link_with_table"});
        if (!with_table) std::fprintf(stderr, "%s\n", with_table.error().c_str());
        pipelines = pipelines && with_table.has_value();

        if (compute && texture) {
            auto group = device.create(fjell::gpu::BindGroupDesc{
                .pipeline = *compute,
                .entries = {{"target", fjell::gpu::storage(fjell::gpu::mip(*texture, 0))}},
                .name = "link_group",
            });
            if (!group) std::fprintf(stderr, "%s\n", group.error().c_str());
            const fjell::gpu::BindEntry refill[] = {{"target", fjell::gpu::storage(fjell::gpu::mip(*texture, 1))}};
            const bool refilled = group && device.update(*group, refill).has_value();
            const bool wrong_kind_refused =
                !device.create(fjell::gpu::BindGroupDesc{
                                   .pipeline = *compute,
                                   .entries = {{"target", fjell::gpu::sampled(*texture, sampler)}}})
                     .has_value();
            pipelines = pipelines && group && refilled && wrong_kind_refused &&
                        fjell::gpu::vulkan::native_group(device, *group) != VK_NULL_HANDLE;
        }
        if (with_table) with_table->reset();

        if (compute) {
            const fjell::gpu::ComputePipeline handle = *compute;
            const auto rebuilt = device.recreate(handle, {.shader = "pipeline.comp"});
            pipelines = pipelines && rebuilt.has_value() && device.layout(handle).push_size == 16;
        }

        if (pipelines && buffer && texture && sampler.valid()) {
            const auto bytes = device.mapped(*buffer);
            std::memset(bytes.data(), 0xAB, bytes.size());
            const VkImageView level = fjell::gpu::vulkan::native_view(device, fjell::gpu::mip(*texture, 1));
            const bool same_sampler =
                device.sampler({.address = fjell::gpu::Address::clamp}) == sampler;
            ran = bytes.size() == 256 && level != VK_NULL_HANDLE && same_sampler &&
                  device.info(*texture).mips == 2;
        }
        core.upload_context().wait_all();
        vkDestroyDescriptorSetLayout(core.vk_device(), table_layout, nullptr);
        if (buffer) buffer->reset();
        if (texture) texture->reset();
        if (compute) compute->reset();
        if (graphics) graphics->reset();
        if (mesh) mesh->reset();
        core.wait_idle();
    }
    fjell::log::shutdown();
    return ran ? 0 : 1;
}
