#include "renderer/pass/meshlet_instance_pass.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "gpu/device.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/hiz_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/resource_registry.hpp"

#include <algorithm>
#include <vector>

namespace fjell {

namespace {

// The expansion and the cull run one invocation per draw and per meshlet.
constexpr uint32_t GROUP = 64;

// A mesh-task dispatch as the indirect commands hold it.
struct TaskDispatch {
    uint32_t x;
    uint32_t y;
    uint32_t z;
};

struct CullPush {
    uint32_t segment_size;
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t hiz_mip_count;
    uint32_t is_second_pass;
};

// A storage buffer bound whole up to `size`, never empty.
gpu::BufferRange at_least_one_word(gpu::Buffer buffer, uint64_t size) {
    return {buffer, 0, std::max<uint64_t>(size, 16)};
}

} // namespace

Result<> MeshletInstancePass::create(Device& /*device*/, const PassResources& resources) {
    device_ = resources.device;
    FJELL_TRY(expand_pipeline_, device_->create(gpu::ComputePipelineDesc{
                                    .shader = "shaders/meshlet_expand.comp",
                                    .name = "meshlet_expand",
                                }));
    // Set 0 the globals, set 2 the draw data the vertex and mesh shaders
    // read the instance transforms from.
    FJELL_TRY(cull_pipeline_, device_->create(gpu::ComputePipelineDesc{
                                  .shader = "shaders/meshlet_cull.comp",
                                  .shared = {resources.globals, resources.draw_data},
                                  .name = "meshlet_cull",
                              }));
    FJELL_TRY(no_hiz_, device_->create(gpu::TextureDesc{
                           .format = gpu::Format::r32_float,
                           .use = gpu::TextureUse::sampled,
                           .initial = gpu::Clear{0.0f},
                           .name = "meshlet_cull_no_hiz",
                       }));

    // Only read with a Hi-Z pass, whose sampler replaces it.
    hiz_sampler_ = device_->sampler({.filter = gpu::Filter::nearest,
                                     .mip_filter = gpu::Filter::nearest,
                                     .address = gpu::Address::clamp});

    FJELL_GFX_DEBUG("MeshletInstancePass created (expand + cull pipelines)");
    return {};
}

void MeshletInstancePass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    if (auto* hiz = passes.try_get<HizPass>("hiz")) hiz_sampler_ = hiz->sampler();
}

void MeshletInstancePass::destroy() {
    expand_pipeline_.reset();
    cull_pipeline_.reset();
    no_hiz_.reset();
    for (auto* buffers : {&instance_buffers_, &expand_counters_, &visible_buffers_,
                          &indirect_buffers_, &culled_buffers_, &culled_counters_}) {
        for (auto& buffer : *buffers) buffer.reset();
    }
    capacities_ = {};
}

void MeshletInstancePass::pre_record(const FrameContext& ctx, GpuContext& /*gpu*/) {
    if (!ctx.indirect_batch) return;
    uint32_t total_mi = ctx.indirect_batch->total_meshlet_instances(ctx.frame_index);
    if (total_mi > 0) {
        ensure_capacity(ctx.frame_index, total_mi);
    }
}

void MeshletInstancePass::ensure_capacity(uint32_t frame_index, uint32_t max_instances) {
    if (max_instances <= capacities_[frame_index]) return;

    uint32_t new_cap = std::max(max_instances, capacities_[frame_index] * 2);
    if (new_cap < 256) new_cap = 256;

    const uint64_t instance_size = uint64_t{new_cap} * sizeof(GpuMeshletInstance);
    bool made_all = true;
    auto make = [&](uint64_t size, gpu::BufferUses use, std::string_view name) {
        auto made = device_->create(gpu::BufferDesc{.size = size, .use = use, .name = name});
        if (!made) {
            FJELL_GFX_ERROR("{}", made.error());
            made_all = false;
            return gpu::Owned<gpu::Buffer>{};
        }
        return std::move(*made);
    };
    auto instances = make(instance_size, gpu::BufferUse::storage, "meshlet_instances");
    // N segments, each able to hold every instance.
    auto visible = make(instance_size * MAX_PIPELINE_BINS, gpu::BufferUse::storage, "meshlet_visible");
    auto expand_count = make(sizeof(uint32_t), gpu::BufferUse::storage, "meshlet_expand_count");
    auto indirect = make(MAX_PIPELINE_BINS * sizeof(TaskDispatch),
                         gpu::BufferUse::storage | gpu::BufferUse::indirect, "meshlet_indirect");
    // What the first cull's Hi-Z test rejected, for the second to test again.
    auto culled = make(instance_size, gpu::BufferUse::storage, "meshlet_culled");
    auto culled_count = make(sizeof(uint32_t), gpu::BufferUse::storage, "meshlet_culled_count");
    if (!made_all) return;

    // The frames still reading the old buffers keep them until they are done.
    instance_buffers_[frame_index] = std::move(instances);
    visible_buffers_[frame_index] = std::move(visible);
    expand_counters_[frame_index] = std::move(expand_count);
    indirect_buffers_[frame_index] = std::move(indirect);
    culled_buffers_[frame_index] = std::move(culled);
    culled_counters_[frame_index] = std::move(culled_count);
    capacities_[frame_index] = new_cap;
}

void MeshletInstancePass::record_expand(const FrameContext& ctx, uint32_t total_draws) {
    const uint32_t frame = ctx.frame_index;
    const gpu::BufferRange instances(instance_buffers_[frame]);
    const gpu::BufferRange count(expand_counters_[frame], 0, sizeof(uint32_t));
    auto& cmd = ctx.commands();

    cmd.fill(count, 0);
    cmd.barrier(count, gpu::Access::copy_dst, gpu::Access::storage_buffer_read_write_compute);

    cmd.set_pipeline(expand_pipeline_);
    cmd.bind({{"DrawDataBuffer",
               gpu::storage(at_least_one_word(ctx.indirect_batch->shared_draw_data_buffer(frame),
                                              ctx.indirect_batch->draw_data_size(frame)))},
              {"InstanceBuffer", gpu::storage(instances)},
              {"CounterBuffer", gpu::storage(count)}});
    cmd.push(total_draws);
    cmd.dispatch((total_draws + GROUP - 1) / GROUP, 1, 1);

    // The cull reads what the expansion wrote, and how much.
    cmd.barrier(instances, gpu::Access::storage_buffer_read_write_compute,
                gpu::Access::storage_buffer_read_compute);
    cmd.barrier(count, gpu::Access::storage_buffer_read_write_compute,
                gpu::Access::storage_buffer_read_compute);
}

void MeshletInstancePass::record_cull(const FrameContext& ctx, Cull which) {
    if (ctx.indirect_batch == nullptr || ctx.target == nullptr || ctx.registry == nullptr) return;
    const uint32_t frame = ctx.frame_index;
    const IndirectBatch& batch = *ctx.indirect_batch;
    if (!visible_buffers_[frame]) return;

    const auto* bounds = ctx.registry->find_buffer("meshlet_bounds");
    if (bounds == nullptr || !bounds->buffer.valid()) return;

    auto* hiz_imgs = ctx.pp_resources ? ctx.pp_resources->hiz() : nullptr;
    const bool hiz_ready = hiz_imgs != nullptr && hiz_imgs->ready();
    if (which == Cull::second && !hiz_ready) return;
    const gpu::Texture hiz = hiz_ready ? gpu::Texture(hiz_imgs->pyramid) : gpu::Texture(no_hiz_);
    const uint32_t hiz_mips = hiz_ready ? hiz_imgs->mip_count : 0;

    auto& cmd = ctx.commands();
    const gpu::BufferRange culled_count(culled_counters_[frame], 0, sizeof(uint32_t));
    if (which == Cull::first) {
        // Every bin's dispatch starts at no groups; the cull counts into x.
        const uint32_t bins = std::max<uint32_t>(static_cast<uint32_t>(batch.pipeline_ranges(frame).size()), 1);
        const std::vector<TaskDispatch> start(bins, TaskDispatch{0, 1, 1});
        const gpu::BufferRange commands(indirect_buffers_[frame], 0, bins * sizeof(TaskDispatch));
        cmd.copy(cmd.transient(start), commands);
        cmd.fill(culled_count, 0);
        cmd.barrier(commands, gpu::Access::copy_dst, gpu::Access::storage_buffer_read_write_compute);
        cmd.barrier(culled_count, gpu::Access::copy_dst, gpu::Access::storage_buffer_read_write_compute);
    }

    // The second cull takes its input from what the first set aside, and
    // appends to the same visible instances and commands.
    const bool first = which == Cull::first;
    const gpu::Buffer input = first ? instance_buffers_[frame] : culled_buffers_[frame];
    const gpu::Buffer input_count = first ? expand_counters_[frame] : culled_counters_[frame];
    const uint32_t segment = capacities_[frame];
    const auto extent = ctx.target->extent();

    cmd.set_pipeline(cull_pipeline_);
    cmd.bind(ctx.globals);
    cmd.bind(ctx.draw_data);
    cmd.bind({{"DrawDataBuffer", gpu::storage(at_least_one_word(batch.shared_draw_data_buffer(frame),
                                                                batch.draw_data_size(frame)))},
              {"InputInstances", gpu::storage(input)},
              {"InputCounter", gpu::storage(gpu::BufferRange(input_count, 0, sizeof(uint32_t)))},
              {"OutputInstances", gpu::storage(visible_buffers_[frame])},
              {"OutputCmds", gpu::storage(indirect_buffers_[frame])},
              {"BoundsBuffer", gpu::storage(at_least_one_word(bounds->buffer, bounds->size))},
              {"PipelineBinBuffer", gpu::storage(at_least_one_word(batch.shared_pipeline_bin_buffer(frame),
                                                                   batch.pipeline_bin_size(frame)))},
              {"hiz_pyramid", gpu::sampled(hiz, hiz_sampler_)},
              {"CulledInstances", gpu::storage(culled_buffers_[frame])},
              {"CulledCounter", gpu::storage(culled_count)}});
    cmd.push(CullPush{
        .segment_size = segment,
        .viewport_width = extent.width,
        .viewport_height = extent.height,
        .hiz_mip_count = hiz_mips,
        .is_second_pass = static_cast<uint32_t>(which),
    });
    cmd.dispatch((segment + GROUP - 1) / GROUP, 1, 1);

    if (first && ctx.render_stats != nullptr) {
        ctx.render_stats->two_pass_occlusion = hiz_ready;
    }
}

PassSetup MeshletInstancePass::setup() const {
    return {
        .name = "meshlet_cull",
        .phase = RenderPhase::DepthPrepass,
        .sub_order = 0,
    };
}

void MeshletInstancePass::collect_exports(DeclareContext& ctx) {
    // This frame slot's cull results. The depth prepass and the colour pass
    // draw from the visible instances and the indirect commands; the second
    // cull appends to both, from the instances this one set aside.
    const uint32_t slot = ctx.frame_index;
    auto add = [&](std::string_view name, gpu::Buffer buffer) {
        if (!buffer.valid()) { return; }
        ctx.buffer_imports.emplace(fg_name_hash(name), BufferImport{.buffer = buffer});
    };
    add("meshlet_visible", visible_buffers_[slot]);
    add("meshlet_indirect", indirect_buffers_[slot]);
    add("meshlet_culled", culled_buffers_[slot]);
    add("meshlet_culled_count", culled_counters_[slot]);
}

void MeshletInstancePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (auto hiz = builder.import_named_optional(ctx, "hiz_pyramid"); hiz.valid()) {
        builder.read(hiz, gpu::Access::sampled_compute);
    }

    // The expansion never leaves this pass, but every viewport in the frame
    // expands into the same slot's buffers, so declaring them orders this
    // viewport's expand after the last one's cull read them. Counters are
    // zeroed by a transfer before the dispatch counts into them.
    const uint32_t slot = ctx.frame_index;
    if (instance_buffers_[slot] && expand_counters_[slot]) {
        auto instances = builder.import("meshlet_instances", instance_buffers_[slot]);
        builder.read_write(instances, gpu::Access::storage_buffer_read_write_compute);
        auto count = builder.import("meshlet_expand_count", expand_counters_[slot]);
        builder.write(count, gpu::Access::copy_dst);
        builder.read_write(count, gpu::Access::storage_buffer_read_write_compute);
    }

    // What the draws read: visible instances appended by atomics, and the
    // indirect commands, uploaded by a transfer and then counted into.
    auto visible = builder.import_named_buffer(ctx, "meshlet_visible");
    builder.read_write(visible, gpu::Access::storage_buffer_read_write_compute);
    auto indirect = builder.import_named_buffer(ctx, "meshlet_indirect");
    builder.write(indirect, gpu::Access::copy_dst);
    builder.read_write(indirect, gpu::Access::storage_buffer_read_write_compute);
    // What the Hi-Z rejected, for the second cull to test again.
    auto culled = builder.import_named_buffer(ctx, "meshlet_culled");
    builder.write(culled, gpu::Access::storage_buffer_write_compute);
    auto culled_count = builder.import_named_buffer(ctx, "meshlet_culled_count");
    builder.write(culled_count, gpu::Access::copy_dst);
    builder.read_write(culled_count, gpu::Access::storage_buffer_read_write_compute);
}

void MeshletInstancePass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_meshlet_instance");
    if (ctx.indirect_batch == nullptr) { return; }

    const uint32_t total_draws = ctx.indirect_batch->opaque_draw_count(ctx.frame_index);
    const uint32_t total_mi = ctx.indirect_batch->total_meshlet_instances(ctx.frame_index);
    if (total_draws == 0 || total_mi == 0) { return; }
    if (!instance_buffers_[ctx.frame_index]) { return; }

    auto zone = ctx.commands().zone("Meshlet Expand+Cull");
    record_expand(ctx, total_draws);
    record_cull(ctx, Cull::first);
}

} // namespace fjell
