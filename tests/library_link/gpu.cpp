// Links fjell-gpu alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its own
// dependencies. Running it opens a window, brings a device up and makes a
// buffer, a texture with a view, a sampler, pipelines, bind groups and
// transient memory through the GPU interface, and records a dispatch, copies,
// clears and draws through command lists, two of them at once on two threads,
// and acceleration structure builds and a ray query where the GPU has them,
// which takes a GPU and a display: it is run by hand, not as a test.

#include "core/log.hpp"
#include "gpu/device.hpp"
#include "gpu/vulkan/device_impl.hpp"
#include "gpu/vulkan/native.hpp"
#include "renderer/gpu/window.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <thread>

int main() {
    fjell::log::init({.level = spdlog::level::warn});
    bool ran = false;
    {
        fjell::Window window("fjell-link-gpu", 320, 240);
        auto made_device = fjell::gpu::Device::create(window);
        if (!made_device) {
            std::fprintf(stderr, "%s\n", made_device.error().c_str());
            return 1;
        }
        fjell::gpu::Device& device = **made_device;
        const VkDevice vk_device = device.impl().device;
        const VkQueue graphics_queue = device.impl().queues[0];
        const uint32_t graphics_family = device.impl().families[0];
        VkCommandPoolCreateInfo one_shot_info{};
        one_shot_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        one_shot_info.queueFamilyIndex = graphics_family;
        VkCommandPool one_shot_pool{VK_NULL_HANDLE};
        vkCreateCommandPool(vk_device, &one_shot_info, nullptr, &one_shot_pool);

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
        // A shared layout the device makes for reflect.comp's set 4 (a table
        // sized at run time and a pair), which lets that pipeline be made
        // once it names it; a persistent group for pipeline.comp's own set,
        // filled and refilled.
        using fjell::gpu::BindingKind;
        auto table = device.create(fjell::gpu::SharedLayoutDesc{
            .name = "table",
            .usual_set = 4,
            .bindings = {{.name = "textures", .binding = 0, .kind = BindingKind::sampled_texture, .count = 16},
                         {.name = "pair", .binding = 1, .kind = BindingKind::sampled_texture, .count = 2}},
        });
        if (!table) std::fprintf(stderr, "%s\n", table.error().c_str());
        auto with_table = device.create(fjell::gpu::ComputePipelineDesc{
            .shader = "reflect.comp", .shared = {table.value_or(fjell::gpu::SharedLayout{})},
            .name = "link_with_table"});
        if (!with_table) std::fprintf(stderr, "%s\n", with_table.error().c_str());
        pipelines = pipelines && table && with_table.has_value();

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

        if (compute) {
            const fjell::gpu::ComputePipeline handle = *compute;
            const auto rebuilt = device.recreate(handle, {.shader = "pipeline.comp"});
            pipelines = pipelines && rebuilt.has_value() && device.layout(handle).push_size == 16;
        }

        // Transient memory, which the command list hands out: two slices in
        // one frame slot, the same ones again when that slot comes round.
        // Frames that submit nothing end at once, so the device can cycle
        // through its slots.
        auto& transient = device.impl().transient;
        auto& first_frame = device.begin_frame();
        auto first = transient.allocate(100);
        auto second = transient.allocate(3u << 20);
        (void)device.end_frame(first_frame);
        auto& second_frame = device.begin_frame();
        auto other_slot = transient.allocate(100);
        (void)device.end_frame(second_frame);
        for (uint32_t slot = 2; slot < device.caps().frames_in_flight; ++slot) {
            (void)device.end_frame(device.begin_frame());
        }
        auto& round_again = device.begin_frame();
        auto again = transient.allocate(100);
        const bool transient_ok = first && second && other_slot && again &&
                                  again->range == first->range &&
                                  other_slot->range.buffer != first->range.buffer &&
                                  second->bytes.size() == (3u << 20) &&
                                  fjell::gpu::vulkan::native_buffer(device, first->range.buffer) != VK_NULL_HANDLE;
        if (first) std::memset(first->bytes.data(), 0xCD, first->bytes.size());
        (void)device.end_frame(round_again);
        if (!transient_ok) std::fprintf(stderr, "transient memory failed\n");
        pipelines = pipelines && transient_ok;

        // Records into a command buffer through a command list, makes what it
        // wrote readable by the CPU, submits it and waits for it.
        auto run = [&](auto&& record) {
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = one_shot_pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            VkCommandBuffer cb{VK_NULL_HANDLE};
            vkAllocateCommandBuffers(vk_device, &allocate, &cb);
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
            vkCreateFence(vk_device, &fence_info, nullptr, &fence);
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cb;
            vkQueueSubmit(graphics_queue, 1, &submit, fence);
            vkWaitForFences(vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkDestroyFence(vk_device, fence, nullptr);
            vkFreeCommandBuffers(vk_device, one_shot_pool, 1, &cb);
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

        // A group of the table, its elements given one at a time. Written in
        // place until a frame binds it; then an update is a new version,
        // and the next, before another bind, goes into that one. Once the
        // frame that bound it is finished, the set is written in place
        // again. What the group already holds costs no version. An element
        // given nothing is emptied; a single binding left empty, or given
        // nothing, is refused.
        bool shared_groups = false;
        if (table && with_table && texture) {
            auto group = device.create(fjell::gpu::SharedGroupDesc{
                .layout = *table,
                .entries = {{"pair", fjell::gpu::sampled(*texture, sampler), 1}},
                .name = "link_table",
            });
            if (!group) std::fprintf(stderr, "%s\n", group.error().c_str());
            auto element = [&](uint32_t at) {
                const fjell::gpu::BindEntry entry[] = {{"textures", fjell::gpu::sampled(*texture, sampler), at}};
                return device.update(*group, entry).has_value();
            };
            auto bind_table = [&](fjell::gpu::CommandList& cmd) {
                cmd.set_pipeline(*with_table);
                cmd.bind(*group);
            };
            // The set as it stands; `native_group` would count as a bind.
            auto current_set = [&] { return device.impl().groups.get(*group)->set; };
            if (group) {
                const VkDescriptorSet made = current_set();
                shared_groups = element(3) && current_set() == made;

                auto& binding_frame = device.begin_frame();
                run(bind_table);
                shared_groups = shared_groups && element(4);
                const VkDescriptorSet version = current_set();
                shared_groups = shared_groups && version != made && element(5) && current_set() == version;
                run(bind_table);
                // The same resource again, the set bound: nothing to write.
                shared_groups = shared_groups && element(5) && current_set() == version;
                (void)device.end_frame(binding_frame);
                vkDeviceWaitIdle(vk_device);
                shared_groups = shared_groups && element(6) && current_set() == version;

                const fjell::gpu::BindEntry emptied[] = {{"textures", fjell::gpu::sampled({}, {}), 5}};
                shared_groups = shared_groups && device.update(*group, emptied).has_value();

                const auto& contents = device.impl().groups.get(*group)->state->contents;
                for (uint32_t at : {3u, 4u, 6u, 17u}) shared_groups = shared_groups && contents[at];
                shared_groups = shared_groups && !contents[0] && !contents[5] && !contents[16];

                const fjell::gpu::BindEntry unknown[] = {{"missing", fjell::gpu::sampled(*texture, sampler)}};
                shared_groups = shared_groups && !device.update(*group, unknown).has_value();
            }
            auto single = device.create(fjell::gpu::SharedLayoutDesc{
                .name = "single",
                .bindings = {{.name = "params", .kind = BindingKind::uniform_buffer}},
            });
            shared_groups = shared_groups && single &&
                            !device.create(fjell::gpu::SharedGroupDesc{.layout = *single}).has_value() &&
                            !device.create(fjell::gpu::SharedGroupDesc{
                                               .layout = *single,
                                               .entries = {{"params", fjell::gpu::uniform({})}}})
                                 .has_value();
        }
        if (!shared_groups) std::fprintf(stderr, "shared groups failed\n");
        pipelines = pipelines && shared_groups;
        if (with_table) with_table->reset();

        // Lists recorded at once on two threads, into secondary command
        // buffers from pools of their own, then played in order by the
        // primary: each dispatch binds transient memory through the shared
        // frame sets and writes its own half of a buffer.
        bool parallel = false;
        auto halves = device.create(fjell::gpu::BufferDesc{
            .size = 512,
            .use = fjell::gpu::BufferUse::storage,
            .memory = fjell::gpu::Memory::readback,
            .name = "link_halves",
        });
        if (commands_pipeline && halves) {
            std::array<VkCommandPool, 2> pools{};
            std::array<VkCommandBuffer, 2> secondaries{};
            for (size_t i = 0; i < 2; ++i) {
                VkCommandPoolCreateInfo pool_info{};
                pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                pool_info.queueFamilyIndex = graphics_family;
                vkCreateCommandPool(vk_device, &pool_info, nullptr, &pools[i]);
                VkCommandBufferAllocateInfo allocate{};
                allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocate.commandPool = pools[i];
                allocate.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
                allocate.commandBufferCount = 1;
                vkAllocateCommandBuffers(vk_device, &allocate, &secondaries[i]);
            }
            fjell::gpu::vulkan::CommandBufferList here(device, secondaries[0]);
            fjell::gpu::vulkan::CommandBufferList there(device, secondaries[1]);
            auto record_half = [&](fjell::gpu::CommandList& cmd, VkCommandBuffer cb, uint32_t half) {
                VkCommandBufferInheritanceInfo inheritance{};
                inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
                VkCommandBufferBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                begin.pInheritanceInfo = &inheritance;
                vkBeginCommandBuffer(cb, &begin);
                struct Push {
                    uint32_t base;
                    uint32_t count;
                };
                const uint32_t scale = 5 + half;
                cmd.set_pipeline(*commands_pipeline);
                cmd.bind({{"params", fjell::gpu::uniform(cmd.transient(scale))},
                          {"results", fjell::gpu::storage(fjell::gpu::BufferRange(*halves, 256 * half, 128))}});
                cmd.push(Push{.base = 100 * half, .count = 32});
                cmd.dispatch(1, 1, 1);
                vkEndCommandBuffer(cb);
            };
            run([&](fjell::gpu::CommandList& cmd) {
                std::thread other([&] { record_half(there.list(), secondaries[1], 1); });
                record_half(here.list(), secondaries[0], 0);
                other.join();
                const std::array<fjell::gpu::CommandList*, 2> lists{&here.list(), &there.list()};
                cmd.execute(lists);
            });
            for (VkCommandPool pool : pools) vkDestroyCommandPool(vk_device, pool, nullptr);
            parallel = true;
            for (uint32_t i = 0; parallel && i < 32; ++i) {
                parallel = word(*halves, i) == 5 * i && word(*halves, 64 + i) == 100 + 6 * i;
            }
        }
        if (!parallel) std::fprintf(stderr, "parallel lists failed\n");
        pipelines = pipelines && parallel;
        if (halves) halves->reset();

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

        // Uploads: words written into a buffer at an offset, a texture written
        // whole with its mips filtered from the first, then a box of it
        // written again; read back once the upload lanes have run. A write
        // past a buffer's end is refused.
        bool uploaded = false;
        auto upload_target = device.create(fjell::gpu::BufferDesc{.size = 32, .name = "link_upload"});
        auto upload_texture = device.create(fjell::gpu::TextureDesc{
            .format = fjell::gpu::Format::rgba8_unorm,
            .width = 4,
            .height = 4,
            .mips = 3,
            .use = fjell::gpu::TextureUse::sampled,
            .name = "link_upload_texture",
        });
        if (upload_target && upload_texture && results) {
            using fjell::gpu::Access;
            constexpr uint32_t RED = 0xFF0000FFU;
            constexpr uint32_t GREEN = 0xFF00FF00U;
            const size_t reported_before = device.impl().reported.size();
            fjell::gpu::Upload& upload = device.upload();
            const std::array<uint32_t, 4> words{1, 2, 3, 4};
            upload.to_buffer(*upload_target, 16, words);
            upload.to_buffer(*upload_target, 24, words);
            std::array<uint32_t, 16> texels{};
            texels.fill(RED);
            upload.to_texture(fjell::gpu::mip(*upload_texture, 0), {},
                              std::as_bytes(std::span(texels)),
                              {.after = Access::sampled_fragment, .generate_mips = true});
            const std::array<uint32_t, 2> box{GREEN, GREEN};
            upload.to_texture(fjell::gpu::mip(*upload_texture, 0),
                              {.x = 1, .y = 2, .width = 2, .height = 1},
                              std::as_bytes(std::span(box)),
                              {.before = Access::sampled_fragment, .after = Access::sampled_fragment});
            device.wait_idle();
            run([&](fjell::gpu::CommandList& cmd) {
                cmd.copy(fjell::gpu::BufferRange(*upload_target, 16, 16),
                         fjell::gpu::BufferRange(*results, 0, 16));
                cmd.barrier(*upload_texture, Access::sampled_fragment, Access::copy_src);
                cmd.copy(fjell::gpu::mip(*upload_texture, 0), fjell::gpu::BufferRange(*results, 16, 64));
                cmd.copy(fjell::gpu::mip(*upload_texture, 2), fjell::gpu::BufferRange(*results, 80, 4));
            });
            uploaded = device.impl().reported.size() == reported_before + 1;
            for (uint32_t i = 0; uploaded && i < 4; ++i) uploaded = word(*results, i) == i + 1;
            for (uint32_t i = 0; uploaded && i < 16; ++i) {
                const bool in_box = i == 9 || i == 10;
                uploaded = word(*results, 4 + i) == (in_box ? GREEN : RED);
            }
            uploaded = uploaded && word(*results, 20) == RED;
        }
        if (!uploaded) std::fprintf(stderr, "uploads failed\n");
        pipelines = pipelines && uploaded;
        if (upload_target) upload_target->reset();
        if (upload_texture) upload_texture->reset();

        // Readbacks: a texture red on its left half and green on its right,
        // with one grey texel, read back whole, as a box, and scaled to half
        // its size. Colour comes back sRGB-encoded, and a read is not ready
        // before it has been sent.
        bool read_back = false;
        auto readable = device.create(fjell::gpu::TextureDesc{
            .format = fjell::gpu::Format::rgba8_unorm,
            .width = 4,
            .height = 4,
            .use = fjell::gpu::TextureUse::sampled,
            .name = "link_readback",
        });
        if (readable) {
            using fjell::gpu::Access;
            constexpr uint32_t RED = 0xFF0000FFU;
            constexpr uint32_t GREEN = 0xFF00FF00U;
            constexpr uint32_t GREY = 0xFF808080U;
            // 128 of 255 read as linear and encoded again.
            constexpr uint32_t GREY_ENCODED = 0xFFBCBCBCU;
            std::array<uint32_t, 16> texels{};
            for (uint32_t i = 0; i < 16; ++i) texels[i] = i % 4 < 2 ? RED : GREEN;
            texels[12] = GREY;
            device.upload().to_texture(*readable, {}, std::as_bytes(std::span(texels)),
                                       {.after = Access::sampled_fragment});
            auto whole = device.read_back(*readable);
            auto box = device.read_back(*readable, {.region = {.x = 2, .width = 2, .height = 4}});
            auto half = device.read_back(*readable, {.width = 2, .height = 2});
            auto pixel = [](const fjell::gpu::Readback& read, uint32_t index) {
                uint32_t value = 0;
                std::memcpy(&value, read.pixels().data() + index * sizeof(uint32_t), sizeof(value));
                return value;
            };
            if (whole && box && half) {
                read_back = !whole->ready();
                whole->wait();
                box->wait();
                half->wait();
                read_back = read_back && whole->ready() && whole->width() == 4 &&
                            box->width() == 2 && half->width() == 2 && half->height() == 2;
                for (uint32_t i = 0; read_back && i < 16; ++i) {
                    read_back = pixel(*whole, i) == (i == 12 ? GREY_ENCODED : texels[i]);
                }
                for (uint32_t i = 0; read_back && i < 8; ++i) read_back = pixel(*box, i) == GREEN;
                read_back = read_back && pixel(*half, 0) == RED && pixel(*half, 1) == GREEN;
            }
            if (!whole) std::fprintf(stderr, "%s\n", whole.error().c_str());
        }
        if (!read_back) std::fprintf(stderr, "readbacks failed\n");
        pipelines = pipelines && read_back;
        if (readable) readable->reset();
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
            // The green triangle is drawn from arguments in a buffer.
            auto draw_args = device.create(fjell::gpu::BufferDesc{
                .size = sizeof(fjell::gpu::DrawArgs), .use = fjell::gpu::BufferUse::indirect,
                .memory = fjell::gpu::Memory::upload, .name = "link_draw_args"});
            if (draw_args) {
                const fjell::gpu::DrawArgs args{.vertex_count = 3, .instance_count = 1};
                std::memcpy(device.mapped(*draw_args).data(), &args, sizeof(args));
            }
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
            if (colour && depth && samples && resolved && drawn && draw_args && vertex_pipeline &&
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
                        pass.draw_indirect(*draw_args, 1, sizeof(fjell::gpu::DrawArgs));
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

        // Acceleration structures, where the GPU has ray queries: a
        // triangle's bottom level and a top level holding it once under a
        // custom index, both built in one list, then two rays traced at it,
        // the first through it and the second beside it.
        bool traced = !device.caps().ray_queries;
        if (device.caps().ray_queries) {
            using fjell::gpu::BufferUse;
            using fjell::gpu::Memory;
            const std::array<float, 9> corners{-1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
            const std::array<uint32_t, 3> order{0, 1, 2};
            auto vertices = device.create(fjell::gpu::BufferDesc{
                .size = sizeof(corners), .use = BufferUse::acceleration_input, .memory = Memory::upload,
                .name = "link_vertices"});
            auto indices = device.create(fjell::gpu::BufferDesc{
                .size = sizeof(order), .use = BufferUse::acceleration_input, .memory = Memory::upload,
                .name = "link_indices"});
            auto records = device.create(fjell::gpu::BufferDesc{
                .size = device.instance_record_size(), .use = BufferUse::acceleration_input,
                .memory = Memory::upload, .name = "link_instances"});
            auto hits = device.create(fjell::gpu::BufferDesc{
                .size = 4 * sizeof(uint32_t), .use = BufferUse::storage, .memory = Memory::readback,
                .name = "link_hits"});
            auto trace = device.create(
                fjell::gpu::ComputePipelineDesc{.shader = "ray_query.comp", .name = "link_ray_query"});
            if (vertices && indices && records && hits && trace) {
                const fjell::gpu::Triangles triangle{.vertices = *vertices,
                                                     .vertex_stride = 3 * sizeof(float),
                                                     .vertex_count = 3,
                                                     .indices = *indices,
                                                     .triangle_count = 1};
                auto bottom = device.create(
                    fjell::gpu::AccelerationStructureDesc{.triangles = triangle, .name = "link_bottom"});
                auto top = device.create(fjell::gpu::AccelerationStructureDesc{
                    .instances = 1, .use = fjell::gpu::AccelerationUse::rebuilt, .name = "link_top"});
                if (bottom && top) {
                    std::memcpy(device.mapped(*vertices).data(), corners.data(), sizeof(corners));
                    std::memcpy(device.mapped(*indices).data(), order.data(), sizeof(order));
                    const fjell::gpu::AccelerationInstance instance{.custom_index = 42, .structure = *bottom};
                    device.write_instances(device.mapped(*records), std::span(&instance, 1));
                    const size_t reported_before = device.impl().reported.size();
                    run([&](fjell::gpu::CommandList& cmd) {
                        using fjell::gpu::Access;
                        auto zone = cmd.zone("link acceleration structures");
                        cmd.build(*bottom, triangle);
                        cmd.barrier(*bottom, Access::acceleration_build, Access::acceleration_build_input);
                        cmd.build(*top, *records, 1);
                        cmd.barrier(*top, Access::acceleration_build, Access::acceleration_trace_compute);
                        cmd.set_pipeline(*trace);
                        cmd.bind({{"scene", fjell::gpu::acceleration(*top)},
                                  {"hits_out", fjell::gpu::storage(*hits)}});
                        cmd.dispatch(1, 1, 1);
                    });
                    traced = device.impl().reported.size() == reported_before && word(*hits, 0) == 1 &&
                             word(*hits, 1) == 42 && word(*hits, 2) == 0;
                }
                if (!bottom) std::fprintf(stderr, "%s\n", bottom.error().c_str());
                if (!top) std::fprintf(stderr, "%s\n", top.error().c_str());
            }
            if (!trace) std::fprintf(stderr, "%s\n", trace.error().c_str());
        }
        if (!traced) std::fprintf(stderr, "acceleration structures failed\n");
        pipelines = pipelines && traced;

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
        device.wait_idle();
        if (buffer) buffer->reset();
        if (texture) texture->reset();
        if (compute) compute->reset();
        if (graphics) graphics->reset();
        if (mesh) mesh->reset();
        device.wait_idle();
        vkDestroyCommandPool(vk_device, one_shot_pool, nullptr);
    }
    fjell::log::shutdown();
    return ran ? 0 : 1;
}
