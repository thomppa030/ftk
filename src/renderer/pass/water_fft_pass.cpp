#include "renderer/pass/water_fft_pass.hpp"

#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "renderer/gpu/deferred_deleter.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/resources/water_spectrum.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace fjell {

namespace {

constexpr float TAU = 6.28318530717958647692f;

// The time the verification sea is held at: late enough that every wavevector
// has turned a different amount, which a check at zero would not exercise.
constexpr float VERIFY_TIME = 3.7f;
// Frames between the copy being recorded and its result being read: long
// enough for every frame in flight when it was recorded to have finished.
constexpr int VERIFY_WAIT_FRAMES = static_cast<int>(MAX_FRAMES_IN_FLIGHT) + 2;

VkImageMemoryBarrier2 mip_barrier(VkImage image, std::uint32_t first_mip, std::uint32_t mip_count,
                                  VkImageLayout old_layout, VkImageLayout new_layout,
                                  VkAccessFlags2 src_access, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = src_access;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, first_mip, mip_count, 0, 1};
    return b;
}

void submit_barriers(VkCommandBuffer cmd, const VkImageMemoryBarrier2* barriers,
                     std::uint32_t count) {
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = count;
    dep.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// One compute dispatch has to finish writing before the next reads it.
void compute_to_compute(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

FrameCacheBinding storage_image(std::uint32_t binding, VkImageView view) {
    FrameCacheBinding b{};
    b.binding = binding;
    b.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b.image.imageView = view;
    b.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    return b;
}

} // namespace

Result<> WaterFftPass::create_pipeline(Pipeline& out, const char* spirv,
                                       const std::vector<VkDescriptorType>& bindings,
                                       std::uint32_t push_size) {
    std::vector<VkDescriptorSetLayoutBinding> layout_bindings(bindings.size());
    for (std::uint32_t i = 0; i < bindings.size(); ++i) {
        layout_bindings[i].binding = i;
        layout_bindings[i].descriptorType = bindings[i];
        layout_bindings[i].descriptorCount = 1;
        layout_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<std::uint32_t>(layout_bindings.size());
    li.pBindings = layout_bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &out.set_layout) != VK_SUCCESS) {
        return make_error("WaterFftPass: failed to create descriptor set layout");
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.size = push_size;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &out.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &pli, nullptr, &out.layout) != VK_SUCCESS) {
        return make_error("WaterFftPass: failed to create pipeline layout");
    }

    auto code = read_spirv(base_path::resolve(spirv));
    auto* module = create_shader_module(device_, code);
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = stage;
    ci.layout = out.layout;
    const VkResult r = vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &ci, nullptr,
                                                &out.pipeline);
    vkDestroyShaderModule(device_, module, nullptr);
    if (r != VK_SUCCESS) {
        return make_error(std::string("WaterFftPass: failed to create pipeline for ") + spirv);
    }
    return {};
}

void WaterFftPass::destroy_pipeline(Pipeline& pipeline) {
    if (pipeline.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline.pipeline, nullptr);
    if (pipeline.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipeline.layout, nullptr);
    if (pipeline.set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, pipeline.set_layout, nullptr);
    }
    pipeline = {};
}

Result<> WaterFftPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    allocator_ = resources.allocator;
    vk_pipeline_cache_ = resources.pipeline_cache;
    deferred_deleter_ = resources.deferred_deleter;

    constexpr auto BUFFER = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    constexpr auto IMAGE = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if (auto r = create_pipeline(evolve_, "shaders/water_fft_evolve.comp.spv",
                                 {BUFFER, IMAGE, IMAGE}, sizeof(EvolvePush)); !r) {
        return r;
    }
    if (auto r = create_pipeline(butterfly_, "shaders/water_fft_butterfly.comp.spv",
                                 {IMAGE, IMAGE, IMAGE, IMAGE}, sizeof(ButterflyPush)); !r) {
        return r;
    }
    if (auto r = create_pipeline(assemble_, "shaders/water_fft_assemble.comp.spv",
                                 {IMAGE, IMAGE, IMAGE, IMAGE}, sizeof(AssemblePush)); !r) {
        return r;
    }

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // The patch tiles: the sea past its edge is the sea at its other edge.
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device_, &si, nullptr, &sampler_) != VK_SUCCESS) {
        return make_error("WaterFftPass: failed to create sampler");
    }

    const char* verify = std::getenv("FJELL_VERIFY_WATER_FFT");
    if (verify != nullptr && verify[0] != '\0' && verify[0] != '0') {
        begin_verification();
    }
    return {};
}

void WaterFftPass::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    verify_readback_.reset();
    for (auto& buffer : amplitude_buffers_) buffer.reset();
    for (auto* set : {&spectra_a_, &spectra_b_, &displacement_}) {
        for (auto& image : *set) {
            if (image.sampled != VK_NULL_HANDLE) vkDestroyImageView(device_, image.sampled, nullptr);
            if (image.top != VK_NULL_HANDLE) vkDestroyImageView(device_, image.top, nullptr);
            destroy_vma_image(allocator_, image.image, image.memory);
            image = {};
        }
    }
    if (derivatives_.sampled != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, derivatives_.sampled, nullptr);
    }
    if (derivatives_.top != VK_NULL_HANDLE) vkDestroyImageView(device_, derivatives_.top, nullptr);
    destroy_vma_image(allocator_, derivatives_.image, derivatives_.memory);
    derivatives_ = {};

    if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    destroy_pipeline(evolve_);
    destroy_pipeline(butterfly_);
    destroy_pipeline(assemble_);
    images_ready_ = false;
    device_ = VK_NULL_HANDLE;
}

Result<> WaterFftPass::create_image(Image& out, VkFormat format, std::uint32_t mips,
                                    VkImageUsageFlags usage, const char* name) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {resolution_, resolution_, 1};
    ii.mipLevels = mips;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (create_vma_image(allocator_, &ii, &out.image, &out.memory) != VK_SUCCESS) {
        return make_error(std::string("WaterFftPass: failed to create image ") + name);
    }
    set_image_debug_name(device_, out.image, name);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    if (vkCreateImageView(device_, &vi, nullptr, &out.sampled) != VK_SUCCESS) {
        return make_error(std::string("WaterFftPass: failed to create view of ") + name);
    }
    vi.subresourceRange.levelCount = 1;
    if (vkCreateImageView(device_, &vi, nullptr, &out.top) != VK_SUCCESS) {
        return make_error(std::string("WaterFftPass: failed to create top view of ") + name);
    }
    return {};
}

void WaterFftPass::retire_image(Image& image) {
    if (image.image == VK_NULL_HANDLE) return;
    // Frames already recorded may still sample it.
    deferred_deleter_->defer([device = device_, allocator = allocator_, doomed = image] {
        if (doomed.sampled != VK_NULL_HANDLE) vkDestroyImageView(device, doomed.sampled, nullptr);
        if (doomed.top != VK_NULL_HANDLE) vkDestroyImageView(device, doomed.top, nullptr);
        destroy_vma_image(allocator, doomed.image, doomed.memory);
    });
    image = {};
}

void WaterFftPass::retire_images() {
    for (auto* set : {&spectra_a_, &spectra_b_, &displacement_}) {
        for (auto& image : *set) retire_image(image);
    }
    retire_image(derivatives_);
    for (auto& buffer : amplitude_buffers_) {
        if (buffer) deferred_deleter_->retire(std::move(buffer));
    }
    images_ready_ = false;
}

Result<> WaterFftPass::build_images() {
    mips_ = static_cast<std::uint32_t>(std::countr_zero(resolution_)) + 1;

    constexpr VkImageUsageFlags SPECTRUM_USAGE = VK_IMAGE_USAGE_STORAGE_BIT;
    constexpr VkImageUsageFlags SURFACE_USAGE =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    for (std::uint32_t i = 0; i < 2; ++i) {
        if (auto r = create_image(spectra_a_[i], SPECTRUM_FORMAT, 1, SPECTRUM_USAGE,
                                  "water_fft_spectra_a"); !r) return r;
        if (auto r = create_image(spectra_b_[i], SPECTRUM_FORMAT, 1, SPECTRUM_USAGE,
                                  "water_fft_spectra_b"); !r) return r;
        if (auto r = create_image(displacement_[i], SURFACE_FORMAT, mips_, SURFACE_USAGE,
                                  "water_fft_displacement"); !r) return r;
    }
    if (auto r = create_image(derivatives_, SURFACE_FORMAT, mips_, SURFACE_USAGE,
                              "water_fft_derivatives"); !r) return r;

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(resolution_) * resolution_
                             * sizeof(glm::vec2);
    for (auto& buffer : amplitude_buffers_) {
        buffer = std::make_unique<Buffer>(allocator_, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          VMA_MEMORY_USAGE_AUTO,
                                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        buffer->map();
    }
    images_fresh_ = true;
    images_ready_ = true;
    return {};
}

void WaterFftPass::set_sea(const std::optional<FftContributor>& sea) {
    if (verifying_) return;   // the verification sea is not to be replaced under it

    if (!sea.has_value()) {
        sea_.reset();
        return;
    }
    if (sea_ == sea) return;

    // Choppiness is applied when the spectrum is turned, each frame, so a
    // change to it alone is not a different drawn sea.
    auto drawn_from = [](FftContributor settings) {
        settings.choppiness = 0.0f;
        return settings;
    };
    const bool redraw = !sea_.has_value() || drawn_from(*sea_) != drawn_from(*sea);

    sea_ = sea;
    if (!redraw) return;

    const water::SpectrumField field = water::generate_spectrum(*sea);
    const std::uint32_t resolution = std::clamp(field.resolution, MIN_RESOLUTION, MAX_RESOLUTION);
    if (field.amplitudes.empty() || resolution != field.resolution) {
        FJELL_CORE_WARN("WaterFftPass: a simulated sea needs a resolution from {} to {}, got {}",
                        MIN_RESOLUTION, MAX_RESOLUTION, sea->resolution);
        sea_.reset();
        return;
    }

    if (resolution != resolution_ || !images_ready_) {
        retire_images();
        resolution_ = resolution;
        if (auto built = build_images(); !built) {
            FJELL_CORE_ERROR("{}", built.error());
            retire_images();
            sea_.reset();
            return;
        }
    }
    k_spacing_ = field.k_spacing;
    amplitudes_ = field.amplitudes;
    amplitudes_stale_.fill(true);
}

void WaterFftPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    stepped_this_frame_ = false;
    time_ = verifying_ ? VERIFY_TIME : ctx.elapsed_time;

    if (verifying_ && verify_frames_left_ > 0 && --verify_frames_left_ == 0) {
        finish_verification();
    }
    if (!active()) return;

    const std::uint32_t slot = ctx.frame_index % MAX_FRAMES_IN_FLIGHT;
    if (amplitudes_stale_[slot]) {
        amplitude_buffers_[slot]->write(amplitudes_.data(),
                                        amplitudes_.size() * sizeof(glm::vec2));
        amplitudes_stale_[slot] = false;
    }
    // This frame writes the surface that was last frame's previous one.
    cur_ = 1 - cur_;
}

PassSetup WaterFftPass::setup() const {
    return {
        .name = "water_fft",
        .phase = RenderPhase::Environment,
        .sub_order = 60,
    };
}

void WaterFftPass::declare(PassBuilder& builder, const DeclareContext& /*ctx*/) {
    // Its images live outside the graph, so nothing the graph can see depends
    // on it; without this it would be culled as dead.
    builder.has_side_effects();
}

void WaterFftPass::generate_mips(VkCommandBuffer cmd, const Image& image) const {
    auto top_to_src = mip_barrier(image.image, 0, 1, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                  VK_ACCESS_2_TRANSFER_READ_BIT);
    submit_barriers(cmd, &top_to_src, 1);

    for (std::uint32_t level = 1; level < mips_; ++level) {
        auto to_dst = mip_barrier(image.image, level, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
        submit_barriers(cmd, &to_dst, 1);

        const auto src_size = static_cast<std::int32_t>(std::max(resolution_ >> (level - 1), 1u));
        const auto dst_size = static_cast<std::int32_t>(std::max(resolution_ >> level, 1u));
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[1] = {src_size, src_size, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[1] = {dst_size, dst_size, 1};
        vkCmdBlitImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        auto to_src = mip_barrier(image.image, level, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        submit_barriers(cmd, &to_src, 1);
    }

    auto to_read = mip_barrier(image.image, 0, mips_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_ACCESS_2_TRANSFER_READ_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    submit_barriers(cmd, &to_read, 1);
}

void WaterFftPass::record(const FrameContext& ctx) {
    if (!active() || stepped_this_frame_) return;
    stepped_this_frame_ = true;

    FJELL_PROFILE_SCOPE_N("record_water_fft");
    VkCommandBuffer cmd = ctx.cmd;
    const std::uint32_t slot = ctx.frame_index % MAX_FRAMES_IN_FLIGHT;
    const std::uint32_t groups = (resolution_ + 7) / 8;
    const auto log2_resolution = static_cast<std::uint32_t>(std::countr_zero(resolution_));

    // Everything written here is written whole, so what it held before is of
    // no interest and every target can start from an undefined layout.
    std::array<VkImageMemoryBarrier2, 6> to_write{};
    std::uint32_t barrier_count = 0;
    for (std::uint32_t i = 0; i < 2; ++i) {
        to_write[barrier_count++] = mip_barrier(spectra_a_[i].image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL, 0,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        to_write[barrier_count++] = mip_barrier(spectra_b_[i].image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL, 0,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    to_write[barrier_count++] = mip_barrier(displacement_[cur_].image, 0, 1,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    to_write[barrier_count++] = mip_barrier(derivatives_.image, 0, 1,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    submit_barriers(cmd, to_write.data(), barrier_count);

    if (images_fresh_) {
        // Nothing has been simulated into last frame's surface yet. Still
        // water is the honest thing for it to hold: the surface then appears
        // to have risen out of a flat sea over one frame, not out of whatever
        // was in the memory.
        const Image& previous = displacement_[1 - cur_];
        auto to_clear = mip_barrier(previous.image, 0, mips_, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        submit_barriers(cmd, &to_clear, 1);
        const VkClearColorValue still{};
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, mips_, 0, 1};
        vkCmdClearColorImage(cmd, previous.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &still, 1,
                             &all);
        auto to_read = mip_barrier(previous.image, 0, mips_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        submit_barriers(cmd, &to_read, 1);
        images_fresh_ = false;
    }

    // Turn the drawn sea to now.
    {
        FrameCacheBinding b[3]{};
        b[0].binding = 0;
        b[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[0].buffer.buffer = amplitude_buffers_[slot]->handle();
        b[0].buffer.range = VK_WHOLE_SIZE;
        b[1] = storage_image(1, spectra_a_[0].top);
        b[2] = storage_image(2, spectra_b_[0].top);
        VkDescriptorSet set = ctx.descriptor_cache->acquire(evolve_.set_layout, {b, 3});

        const EvolvePush push{resolution_, log2_resolution, k_spacing_, time_, sea_->choppiness};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, evolve_.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, evolve_.layout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(cmd, evolve_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, groups, groups, 1);
    }

    // Transform: every stage along x, then every stage along z.
    std::uint32_t source = 0;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, butterfly_.pipeline);
    for (std::uint32_t vertical = 0; vertical < 2; ++vertical) {
        for (std::uint32_t stage = 1; stage <= log2_resolution; ++stage) {
            compute_to_compute(cmd);

            const FrameCacheBinding b[4]{
                storage_image(0, spectra_a_[source].top),
                storage_image(1, spectra_b_[source].top),
                storage_image(2, spectra_a_[1 - source].top),
                storage_image(3, spectra_b_[1 - source].top),
            };
            VkDescriptorSet set = ctx.descriptor_cache->acquire(butterfly_.set_layout, {b, 4});

            const ButterflyPush push{resolution_, stage, vertical};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, butterfly_.layout, 0, 1,
                                    &set, 0, nullptr);
            vkCmdPushConstants(cmd, butterfly_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(push), &push);
            vkCmdDispatch(cmd, groups, groups, 1);
            source = 1 - source;
        }
    }

    // Gather the surface and how it changes across the patch.
    {
        compute_to_compute(cmd);
        const FrameCacheBinding b[4]{
            storage_image(0, spectra_a_[source].top),
            storage_image(1, spectra_b_[source].top),
            storage_image(2, displacement_[cur_].top),
            storage_image(3, derivatives_.top),
        };
        VkDescriptorSet set = ctx.descriptor_cache->acquire(assemble_.set_layout, {b, 4});

        const AssemblePush push{resolution_, sea_->patch_size / static_cast<float>(resolution_)};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, assemble_.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, assemble_.layout, 0, 1, &set,
                                0, nullptr);
        vkCmdPushConstants(cmd, assemble_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(cmd, groups, groups, 1);
    }

    if (verifying_ && verify_frames_left_ < 0) {
        record_verification_copy(cmd);
    }

    generate_mips(cmd, displacement_[cur_]);
    generate_mips(cmd, derivatives_);
}

void WaterFftPass::begin_verification() {
    FftContributor sea;
    sea.wind_direction = {0.8f, 0.6f};
    sea.wind_speed = 9.0f;
    sea.patch_size = 96.0f;
    // Small enough that the plain Fourier sum it is held to takes a moment
    // rather than minutes, large enough to run every kind of butterfly stage.
    sea.resolution = 64;
    sea.choppiness = 1.2f;
    sea.seed = 7;
    set_sea(sea);
    verifying_ = true;
    FJELL_CORE_INFO("WaterFftPass: verifying the GPU transform against the CPU reference");
}

void WaterFftPass::record_verification_copy(VkCommandBuffer cmd) {
    const VkDeviceSize texels = static_cast<VkDeviceSize>(resolution_) * resolution_;
    const VkDeviceSize image_bytes = texels * 4 * sizeof(std::uint16_t);
    verify_readback_ = std::make_unique<Buffer>(
        allocator_, image_bytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
    verify_readback_->map();

    // The assemble pass has to have finished before its images are copied.
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {resolution_, resolution_, 1};
    vkCmdCopyImageToBuffer(cmd, displacement_[cur_].image, VK_IMAGE_LAYOUT_GENERAL,
                           verify_readback_->handle(), 1, &region);
    region.bufferOffset = image_bytes;
    vkCmdCopyImageToBuffer(cmd, derivatives_.image, VK_IMAGE_LAYOUT_GENERAL,
                           verify_readback_->handle(), 1, &region);

    verify_frames_left_ = VERIFY_WAIT_FRAMES;
}

void WaterFftPass::finish_verification() {
    const std::uint32_t n = resolution_;
    const water::SpectrumField field = water::generate_spectrum(*sea_);
    const water::DisplacementSpectra spectra =
        water::evolve_spectrum(field, sea_->choppiness, VERIFY_TIME);
    const std::vector<float> height = water::inverse_transform_reference(spectra.height, n);
    const std::vector<float> along_x = water::inverse_transform_reference(spectra.along_x, n);
    const std::vector<float> along_z = water::inverse_transform_reference(spectra.along_z, n);

    const auto* read = static_cast<const std::uint16_t*>(verify_readback_->mapped());
    const std::size_t texels = static_cast<std::size_t>(n) * n;
    const float across = 1.0f / (2.0f * sea_->patch_size / static_cast<float>(n));
    auto at = [n](const std::vector<float>& v, std::uint32_t x, std::uint32_t z) {
        return v[static_cast<std::size_t>(z % n) * n + x % n];
    };

    float worst_surface = 0.0f;
    float worst_slope = 0.0f;
    float tallest = 0.0f;
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const std::size_t i = (static_cast<std::size_t>(z) * n + x) * 4;
            const std::size_t d = i + texels * 4;
            auto half = [&](std::size_t index) { return glm::unpackHalf1x16(read[index]); };

            tallest = std::max(tallest, std::abs(at(height, x, z)));
            worst_surface = std::max({worst_surface,
                                      std::abs(half(i + 0) - at(along_x, x, z)),
                                      std::abs(half(i + 1) - at(height, x, z)),
                                      std::abs(half(i + 2) - at(along_z, x, z))});

            // The same differences the shader takes, of the CPU's surface.
            const float dh_dx = (at(height, x + 1, z) - at(height, x + n - 1, z)) * across;
            const float dh_dz = (at(height, x, z + 1) - at(height, x, z + n - 1)) * across;
            const float dx_dx = (at(along_x, x + 1, z) - at(along_x, x + n - 1, z)) * across;
            const float dz_dz = (at(along_z, x, z + 1) - at(along_z, x, z + n - 1)) * across;
            const float dx_dz = (at(along_x, x, z + 1) - at(along_x, x, z + n - 1)) * across;
            worst_slope = std::max({worst_slope,
                                    std::abs(half(d + 0) - dh_dx), std::abs(half(d + 1) - dh_dz),
                                    std::abs(half(d + 2) - dx_dx), std::abs(half(d + 3) - dz_dz),
                                    std::abs(half(i + 3) - dx_dz)});
        }
    }

    // A 16-bit float holds about three and a half digits, so the surface can
    // be off by a thousandth of its height and no closer; a wrong transform
    // is off by the height itself.
    const float allowed = std::max(tallest, 0.1f) * 2e-3f;
    if (worst_surface <= allowed && worst_slope <= allowed * 4.0f) {
        FJELL_CORE_INFO("WaterFftPass: VERIFIED. {}x{} sea up to {:.3f} m: surface within "
                        "{:.5f} m, derivatives within {:.5f}", n, n, tallest, worst_surface,
                        worst_slope);
    } else {
        FJELL_CORE_ERROR("WaterFftPass: VERIFICATION FAILED. {}x{} sea up to {:.3f} m: surface "
                         "off by {:.5f} m, derivatives by {:.5f} (allowed {:.5f})", n, n, tallest,
                         worst_surface, worst_slope, allowed);
    }
    verify_readback_.reset();
}

} // namespace fjell
