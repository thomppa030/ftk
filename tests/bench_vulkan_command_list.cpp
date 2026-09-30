// Times recording through a command list against recording the same commands
// straight into Vulkan, on a real device: 10,000 times a compute pipeline
// set, its one set bound, push data and a dispatch. The set is bound two ways:
// a persistent group made once, and resources by name, which the frame's
// descriptor sets serve after the first bind. Run by hand in an optimised
// build (build-release); it prints nanoseconds per dispatch, the median and
// the fastest of many runs taken in turn so each way sees the same machine,
// then what binding by name spends that Vulkan does not, step by step.
//
// Given part of a way's name (`fjell-bench-command-list "bound by name"`), it
// records only the ways that match, ten times as often, for a profiler.

#include "core/log.hpp"
#include "gpu/command_list.hpp"
#include "gpu/device.hpp"
#include "gpu/vulkan/device_impl.hpp"
#include "gpu/vulkan/native.hpp"
#include "gpu/window.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr uint32_t DISPATCHES = 10'000;
constexpr int RUNS = 31;

struct Push {
    uint32_t base;
    uint32_t count;
};

struct Way {
    const char* name;
    std::function<void(VkCommandBuffer)> record;
    std::vector<double> ns_per_dispatch;
};

// The median of `runs` timings of `step` done DISPATCHES times, in ns per step.
double median_ns(const std::function<void()>& step) {
    std::vector<double> ns;
    for (int run = 0; run < RUNS; ++run) {
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < DISPATCHES; ++i) step();
        const auto stop = std::chrono::steady_clock::now();
        ns.push_back(std::chrono::duration<double, std::nano>(stop - start).count() / DISPATCHES);
    }
    std::ranges::sort(ns);
    return ns[ns.size() / 2];
}

} // namespace

int main(int argc, char** argv) {
    fjell::log::init({.level = spdlog::level::warn});
    namespace gpu = fjell::gpu;
    int status = 1;
    {
        fjell::Window window("fjell-bench-command-list", 320, 240);
        auto made_device = gpu::Device::create(window);
        if (!made_device) {
            std::fprintf(stderr, "%s\n", made_device.error().c_str());
            return 1;
        }
        gpu::Device& device = **made_device;
        device.set_shader_locator([](const std::string& relative) {
            return std::string(FJELL_TEST_SHADER_DIR "/") + relative;
        });

        auto pipeline = device.create(gpu::ComputePipelineDesc{.shader = "commands.comp"});
        auto params = device.create(gpu::BufferDesc{
            .size = 256, .use = gpu::BufferUse::uniform, .memory = gpu::Memory::upload});
        auto results = device.create(gpu::BufferDesc{.size = 256, .use = gpu::BufferUse::storage});
        if (!pipeline || !params || !results) {
            std::fprintf(stderr, "setup failed\n");
            return 1;
        }
        auto group = device.create(gpu::BindGroupDesc{
            .pipeline = *pipeline,
            .entries = {{"params", gpu::uniform(*params)}, {"results", gpu::storage(*results)}},
        });
        if (!group) {
            std::fprintf(stderr, "%s\n", group.error().c_str());
            return 1;
        }

        // What the raw Vulkan ways bind, taken from what the interface made.
        const VkPipeline native_pipeline = gpu::vulkan::native_pipeline(device, *pipeline);
        const VkPipelineLayout layout = gpu::vulkan::native_layout(device, *pipeline);
        const VkDescriptorSet persistent_set = gpu::vulkan::native_group(device, *group);
        const VkDescriptorSetLayout set_layout =
            device.impl().compute_pipelines.get(*pipeline)->layout.set_layouts[0];
        std::array<fjell::FrameCacheBinding, 2> bindings{};
        bindings[0].binding = 0;
        bindings[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].buffer = {gpu::vulkan::native_buffer(device, *params), 0, VK_WHOLE_SIZE};
        bindings[1].binding = 1;
        bindings[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].buffer = {gpu::vulkan::native_buffer(device, *results), 0, VK_WHOLE_SIZE};
        fjell::FrameDescriptorCache& frame_sets = gpu::vulkan::frame_cache(device);
        const Push push{.base = 1, .count = 64};

        std::vector<Way> ways;
        ways.push_back({"Vulkan, persistent set", [&](VkCommandBuffer cb) {
            for (uint32_t i = 0; i < DISPATCHES; ++i) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, native_pipeline);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
                                        &persistent_set, 0, nullptr);
                vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
                vkCmdDispatch(cb, 1, 1, 1);
            }
        }, {}});
        ways.push_back({"Command list, persistent group", [&](VkCommandBuffer cb) {
            gpu::vulkan::CommandBufferList commands(device, cb);
            gpu::CommandList& cmd = commands.list();
            for (uint32_t i = 0; i < DISPATCHES; ++i) {
                cmd.set_pipeline(*pipeline);
                cmd.bind(*group);
                cmd.push(push);
                cmd.dispatch(1, 1, 1);
            }
        }, {}});
        ways.push_back({"Vulkan, frame descriptor set", [&](VkCommandBuffer cb) {
            for (uint32_t i = 0; i < DISPATCHES; ++i) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, native_pipeline);
                const VkDescriptorSet set = frame_sets.acquire(set_layout, bindings);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0,
                                        nullptr);
                vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
                vkCmdDispatch(cb, 1, 1, 1);
            }
        }, {}});
        ways.push_back({"Command list, bound by name", [&](VkCommandBuffer cb) {
            gpu::vulkan::CommandBufferList commands(device, cb);
            gpu::CommandList& cmd = commands.list();
            for (uint32_t i = 0; i < DISPATCHES; ++i) {
                cmd.set_pipeline(*pipeline);
                cmd.bind({{"params", gpu::uniform(*params)}, {"results", gpu::storage(*results)}});
                cmd.push(push);
                cmd.dispatch(1, 1, 1);
            }
        }, {}});

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = device.impl().families[0];
        VkCommandPool pool{VK_NULL_HANDLE};
        vkCreateCommandPool(device.impl().device, &pool_info, nullptr, &pool);
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer cb{VK_NULL_HANDLE};
        vkAllocateCommandBuffers(device.impl().device, &allocate, &cb);

        // Only the ways asked for, longer, when profiling.
        const bool profiling = argc > 1;
        if (profiling) {
            std::erase_if(ways, [&](const Way& way) {
                return std::string_view(way.name).find(argv[1]) == std::string_view::npos;
            });
        }
        const int runs = profiling ? RUNS * 10 : RUNS;

        const size_t reported_before = device.impl().reported.size();
        for (int run = 0; run < runs; ++run) {
            for (Way& way : ways) {
                // A frame of its own each time: the frame sets start empty.
                // It submits nothing; the way records into a buffer of its own.
                gpu::Frame& frame = device.begin_frame();
                vkResetCommandBuffer(cb, 0);
                VkCommandBufferBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cb, &begin);
                const auto start = std::chrono::steady_clock::now();
                way.record(cb);
                const auto stop = std::chrono::steady_clock::now();
                vkEndCommandBuffer(cb);
                (void)device.end_frame(frame);
                const double ns = std::chrono::duration<double, std::nano>(stop - start).count();
                way.ns_per_dispatch.push_back(ns / DISPATCHES);
            }
        }
        vkDestroyCommandPool(device.impl().device, pool, nullptr);

        if (device.impl().reported.size() != reported_before) {
            std::fprintf(stderr, "the command list refused something; the times mean nothing\n");
        } else {
            std::printf("%u dispatches, %d runs: ns per pipeline + bind + push + dispatch\n",
                        DISPATCHES, runs);
            for (Way& way : ways) {
                std::ranges::sort(way.ns_per_dispatch);
                std::printf("  %-32s median %6.1f   fastest %6.1f\n", way.name,
                            way.ns_per_dispatch[way.ns_per_dispatch.size() / 2],
                            way.ns_per_dispatch.front());
            }

            status = 0;
        }
        if (status == 0 && !profiling) {
            // The steps of a bind by name the Vulkan way does not take.
            const gpu::ShaderLayout& shader_layout = device.layout(*pipeline);
            const std::array<gpu::BindEntry, 2> entries{
                gpu::BindEntry{"params", gpu::uniform(*params)},
                gpu::BindEntry{"results", gpu::storage(*results)}};
            const auto placed = gpu::place(shader_layout, entries);
            std::printf("binding by name, ns per bind:\n");
            std::printf("  %-32s median %6.1f\n", "place() the entries by name",
                        median_ns([&] { (void)gpu::place(shader_layout, entries); }));
            std::printf("  %-32s median %6.1f\n", "check the resources exist",
                        median_ns([&] { (void)device.impl().check_resources(*placed); }));
            std::printf("  %-32s median %6.1f\n", "describe the descriptors", median_ns([&] {
                            std::vector<fjell::FrameCacheBinding> described;
                            described.reserve(placed->entries.size());
                            for (const auto& entry : placed->entries) {
                                described.push_back(device.impl().describe(entry));
                            }
                        }));
            std::printf("  %-32s median %6.1f\n", "find the frame set (both ways)",
                        median_ns([&] { (void)frame_sets.acquire(set_layout, bindings); }));
            std::printf("  %-32s median %6.1f\n", "all four in turn", median_ns([&] {
                            auto in_turn = gpu::place(shader_layout, entries);
                            (void)device.impl().check_resources(*in_turn);
                            std::vector<fjell::FrameCacheBinding> described;
                            described.reserve(in_turn->entries.size());
                            for (const auto& entry : in_turn->entries) {
                                described.push_back(device.impl().describe(entry));
                            }
                            (void)frame_sets.acquire(set_layout, described);
                        }));
        }
        device.wait_idle();
    }
    fjell::log::shutdown();
    return status;
}
