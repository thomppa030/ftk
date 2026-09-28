// Links fjell-gpu alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its own
// dependencies. Running it opens a window, brings a device up and makes a
// buffer, a texture with a view, a sampler, pipelines, bind groups and
// transient memory through the GPU interface, and records a dispatch, copies,
// clears and draws through a command list, which takes a GPU and a display: it
// is run by hand, not as a test.

#include "core/log.hpp"
#include "gpu/device.hpp"
#include "gpu/vulkan/device_impl.hpp"
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

        // Transient memory, which the command list hands out: two slices in
        // one frame slot, the same ones again when that slot comes round.
        auto& transient = device.impl().transient;
        fjell::gpu::vulkan::begin_frame(device, 0);
        auto first = transient.allocate(100);
        auto second = transient.allocate(3u << 20);
        fjell::gpu::vulkan::begin_frame(device, 1);
        auto other_slot = transient.allocate(100);
        fjell::gpu::vulkan::begin_frame(device, 0);
        auto again = transient.allocate(100);
        const bool transient_ok = first && second && other_slot && again &&
                                  again->range == first->range &&
                                  other_slot->range.buffer != first->range.buffer &&
                                  second->bytes.size() == (3u << 20) &&
                                  fjell::gpu::vulkan::native_buffer(device, first->range.buffer) != VK_NULL_HANDLE;
        if (first) std::memset(first->bytes.data(), 0xCD, first->bytes.size());
        if (!transient_ok) std::fprintf(stderr, "transient memory failed\n");
        pipelines = pipelines && transient_ok;

        // Records into a command buffer through a command list, makes what it
        // wrote readable by the CPU, submits it and waits for it.
        auto run = [&](auto&& record) {
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = core.command_pool();
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            VkCommandBuffer cb{VK_NULL_HANDLE};
            vkAllocateCommandBuffers(core.vk_device(), &allocate, &cb);
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cb, &begin);
            {
                fjell::gpu::vulkan::CommandBufferList commands(device, cb);
                record(commands.list());
            }
            VkMemoryBarrier2 written{};
            written.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            written.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            written.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
            written.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            written.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &written;
            vkCmdPipelineBarrier2(cb, &dependency);
            vkEndCommandBuffer(cb);

            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence fence{VK_NULL_HANDLE};
            vkCreateFence(core.vk_device(), &fence_info, nullptr, &fence);
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cb;
            vkQueueSubmit(core.graphics_queue(), 1, &submit, fence);
            vkWaitForFences(core.vk_device(), 1, &fence, VK_TRUE, UINT64_MAX);
            vkDestroyFence(core.vk_device(), fence, nullptr);
            vkFreeCommandBuffers(core.vk_device(), core.command_pool(), 1, &cb);
        };
        auto word = [&](fjell::gpu::Buffer read_back, uint32_t index) {
            uint32_t value = 0;
            std::memcpy(&value, device.mapped(read_back).data() + index * sizeof(uint32_t), sizeof(value));
            return value;
        };

        // A dispatch: a pipeline, a uniform in transient memory, a buffer the
        // CPU reads back, push data. Then a second with nothing bound, which
        // the list refuses.
        bool recorded = false;
        auto commands_pipeline = device.create(
            fjell::gpu::ComputePipelineDesc{.shader = "commands.comp", .name = "link_commands"});
        auto results = device.create(fjell::gpu::BufferDesc{
            .size = 64 * sizeof(uint32_t),
            .use = fjell::gpu::BufferUse::storage,
            .memory = fjell::gpu::Memory::readback,
            .name = "link_results",
        });
        if (commands_pipeline && results) {
            const size_t reported_before = device.impl().reported.size();
            run([&](fjell::gpu::CommandList& cmd) {
                struct Push {
                    uint32_t base;
                    uint32_t count;
                };
                const uint32_t scale = 3;
                auto zone = cmd.zone("link dispatch");
                cmd.set_pipeline(*commands_pipeline);
                cmd.bind({{"params", fjell::gpu::uniform(cmd.transient(scale))},
                          {"results", fjell::gpu::storage(*results)}});
                cmd.push(Push{.base = 7, .count = 64});
                cmd.dispatch(1, 1, 1);
                cmd.set_pipeline(*commands_pipeline);
                cmd.dispatch(1, 1, 1);
            });
            recorded = device.impl().reported.size() == reported_before + 1;
            for (uint32_t i = 0; recorded && i < 64; ++i) recorded = word(*results, i) == 7 + 3 * i;
        }
        if (!commands_pipeline) std::fprintf(stderr, "%s\n", commands_pipeline.error().c_str());
        if (!recorded) std::fprintf(stderr, "command list failed\n");
        pipelines = pipelines && recorded;

        // Copies and clears: a buffer filled and copied; a texture cleared,
        // its mips filtered down from the first and the smallest read back.
        bool copied = false;
        auto source = device.create(fjell::gpu::BufferDesc{.size = 16, .name = "link_fill"});
        auto mipped = device.create(fjell::gpu::TextureDesc{
            .format = fjell::gpu::Format::rgba8_unorm,
            .width = 4,
            .height = 4,
            .mips = 3,
            .use = fjell::gpu::TextureUse::sampled,
            .name = "link_mips",
        });
        if (source && mipped && results) {
            using fjell::gpu::Access;
            const size_t reported_before = device.impl().reported.size();
            run([&](fjell::gpu::CommandList& cmd) {
                cmd.fill(*source, 0xABCD1234U);
                cmd.barrier(*source, Access::clear, Access::copy_src);
                cmd.copy(*source, fjell::gpu::BufferRange(*results, 0, 16));

                cmd.barrier(*mipped, {}, Access::clear);
                cmd.clear(fjell::gpu::mip(*mipped, 0), fjell::gpu::Clear{1.0f, 0.0f, 1.0f, 1.0f});
                cmd.barrier(*mipped, Access::clear, Access::copy_dst);
                cmd.generate_mipmaps(*mipped);
                cmd.barrier(fjell::gpu::mip(*mipped, 2), Access::copy_dst, Access::copy_src);
                cmd.copy(fjell::gpu::mip(*mipped, 2), fjell::gpu::BufferRange(*results, 16, 4));
            });
            copied = device.impl().reported.size() == reported_before;
            for (uint32_t i = 0; copied && i < 4; ++i) copied = word(*results, i) == 0xABCD1234U;
            copied = copied && word(*results, 4) == 0xFFFF00FFU;
        }
        if (!copied) std::fprintf(stderr, "copies and clears failed\n");
        pipelines = pipelines && copied;
        // Rendering: a triangle over a 4 × 4 target drawn with vertices and
        // tested against depth, then with a mesh shader where the GPU has one,
        // then into four samples resolved into one; each target read back.
        // A pipeline for other targets and a copy inside the scope are refused.
        bool rendered = false;
        {
            using fjell::gpu::Access;
            using fjell::gpu::Format;
            auto target = [&](Format format, fjell::gpu::TextureUse use, fjell::gpu::Samples samples) {
                return device.create(fjell::gpu::TextureDesc{
                    .format = format, .width = 4, .height = 4, .samples = samples, .use = use});
            };
            const auto x1 = fjell::gpu::Samples::x1;
            const auto x4 = fjell::gpu::Samples::x4;
            auto colour = target(Format::rgba8_unorm, fjell::gpu::TextureUse::color_target, x1);
            auto depth = target(Format::d32_float, fjell::gpu::TextureUse::depth_target, x1);
            auto samples = target(Format::rgba8_unorm, fjell::gpu::TextureUse::color_target, x4);
            auto resolved = target(Format::rgba8_unorm, fjell::gpu::TextureUse::color_target, x1);
            auto drawn = device.create(fjell::gpu::BufferDesc{
                .size = 3 * 64, .memory = fjell::gpu::Memory::readback, .name = "link_drawn"});
            auto vertex_pipeline = device.create(fjell::gpu::GraphicsPipelineDesc{
                .vertex = "draw.vert",
                .fragment = "draw.frag",
                .depth = {.test = true, .write = true},
                .color = {{Format::rgba8_unorm}},
                .depth_format = Format::d32_float,
                .name = "link_draw"});
            const bool has_mesh = device.caps().mesh_max_output_vertices > 0;
            auto mesh_pipeline = device.create(fjell::gpu::GraphicsPipelineDesc{
                .mesh = "draw.mesh",
                .fragment = "draw.frag",
                .color = {{Format::rgba8_unorm}},
                .name = "link_draw_mesh"});
            auto sampled_pipeline = device.create(fjell::gpu::GraphicsPipelineDesc{
                .vertex = "draw.vert",
                .fragment = "draw.frag",
                .color = {{Format::rgba8_unorm}},
                .samples = x4,
                .name = "link_draw_samples"});
            struct Colour {
                float r, g, b, a;
            };
            if (colour && depth && samples && resolved && drawn && vertex_pipeline &&
                sampled_pipeline && (mesh_pipeline || !has_mesh)) {
                const size_t reported_before = device.impl().reported.size();
                run([&](fjell::gpu::CommandList& cmd) {
                    cmd.barrier(*colour, {}, Access::color_attachment);
                    cmd.barrier(*depth, {}, Access::depth_attachment);
                    {
                        auto pass = cmd.render({
                            .color = {{.view = *colour, .load = fjell::gpu::Load::clear}},
                            .depth = fjell::gpu::DepthAttachment{.view = *depth,
                                                                 .load = fjell::gpu::Load::clear},
                        });
                        pass.set_pipeline(*sampled_pipeline);
                        pass.draw(3);
                        cmd.copy(*source, *drawn);
                        pass.set_pipeline(*vertex_pipeline);
                        pass.push(Colour{0.0f, 1.0f, 0.0f, 1.0f});
                        pass.draw(3);
                    }
                    cmd.barrier(*colour, Access::color_attachment, Access::copy_src);
                    cmd.copy(*colour, fjell::gpu::BufferRange(*drawn, 0, 64));

                    if (has_mesh) {
                        cmd.barrier(*colour, Access::copy_src, Access::color_attachment);
                        {
                            auto pass = cmd.render({.color = {{.view = *colour}}});
                            auto zone = cmd.zone("link mesh draw");
                            pass.set_pipeline(*mesh_pipeline);
                            pass.push(Colour{0.0f, 0.0f, 1.0f, 1.0f});
                            pass.draw_mesh_tasks(1, 1, 1);
                        }
                        cmd.barrier(*colour, Access::color_attachment, Access::copy_src);
                        cmd.copy(*colour, fjell::gpu::BufferRange(*drawn, 64, 64));
                    }

                    cmd.barrier(*samples, {}, Access::color_attachment);
                    cmd.barrier(*resolved, {}, Access::color_attachment);
                    {
                        auto pass = cmd.render({.color = {{.view = *samples,
                                                           .load = fjell::gpu::Load::clear,
                                                           .store = fjell::gpu::Store::discard,
                                                           .resolve = *resolved}}});
                        pass.set_pipeline(*sampled_pipeline);
                        pass.push(Colour{1.0f, 0.0f, 0.0f, 1.0f});
                        pass.draw(3);
                    }
                    cmd.barrier(*resolved, Access::color_attachment, Access::copy_src);
                    cmd.copy(*resolved, fjell::gpu::BufferRange(*drawn, 128, 64));
                });
                // The pipeline for other targets and the copy in the scope.
                rendered = device.impl().reported.size() == reported_before + 2;
                for (uint32_t i = 0; rendered && i < 16; ++i) {
                    rendered = word(*drawn, i) == 0xFF00FF00U &&
                               (!has_mesh || word(*drawn, 16 + i) == 0xFFFF0000U) &&
                               word(*drawn, 32 + i) == 0xFF0000FFU;
                }
            }
            if (!vertex_pipeline) std::fprintf(stderr, "%s\n", vertex_pipeline.error().c_str());
            if (has_mesh && !mesh_pipeline) std::fprintf(stderr, "%s\n", mesh_pipeline.error().c_str());
            if (!sampled_pipeline) std::fprintf(stderr, "%s\n", sampled_pipeline.error().c_str());
        }
        if (!rendered) std::fprintf(stderr, "rendering failed\n");
        pipelines = pipelines && rendered;

        if (commands_pipeline) commands_pipeline->reset();
        if (results) results->reset();
        if (source) source->reset();
        if (mipped) mipped->reset();

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
