#include "renderer/pass/water_fft_pass.hpp"
#include "gpu/vulkan/native.hpp"

#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/shader_utils.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/resources/water_spectrum.hpp"
#include "renderer/resources/water_surface.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
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
// Where the surface is sampled: the water's task and mesh stages, and the
// waterline and fog in compute. A reader anywhere else declares its read and
// gets its own barrier from the graph.
constexpr VkPipelineStageFlags2 SURFACE_READERS = VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT
                                                | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT
                                                | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
// The same readers as accesses: where the surfaces rest.
constexpr gpu::AccessSet SURFACE_READS = gpu::Access::sampled_mesh | gpu::Access::sampled_compute;

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
    gpu_device_ = resources.device;
    allocator_ = resources.allocator;
    vk_pipeline_cache_ = resources.pipeline_cache;

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

    if (auto r = create_image(still_, SURFACE_FORMAT, 1, 1,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              "water_fft_still", &still_texture_); !r) {
        return r;
    }
    if (auto r = seed_still(resources); !r) {
        return r;
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
    displacement_textures_ = {};
    derivatives_texture_.reset();
    still_texture_.reset();
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
    if (still_.sampled != VK_NULL_HANDLE) vkDestroyImageView(device_, still_.sampled, nullptr);
    if (still_.top != VK_NULL_HANDLE) vkDestroyImageView(device_, still_.top, nullptr);
    destroy_vma_image(allocator_, still_.image, still_.memory);
    still_ = {};

    if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    destroy_pipeline(evolve_);
    destroy_pipeline(butterfly_);
    destroy_pipeline(assemble_);
    images_ready_ = false;
    device_ = VK_NULL_HANDLE;
}

Result<> WaterFftPass::create_image(Image& out, VkFormat format, std::uint32_t size,
                                    std::uint32_t mips, VkImageUsageFlags usage,
                                    const char* name, gpu::Owned<gpu::Texture>* texture) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {size, size, 1};
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
    if (texture != nullptr) *texture = gpu::vulkan::adopt(*gpu_device_, out.image, out.sampled, ii);
    vi.subresourceRange.levelCount = 1;
    if (vkCreateImageView(device_, &vi, nullptr, &out.top) != VK_SUCCESS) {
        return make_error(std::string("WaterFftPass: failed to create top view of ") + name);
    }
    return {};
}

void WaterFftPass::retire_image(Image& image) {
    if (image.image == VK_NULL_HANDLE) return;
    // Frames already recorded may still sample it.
    gpu::vulkan::defer(*gpu_device_, [device = device_, allocator = allocator_, doomed = image] {
        if (doomed.sampled != VK_NULL_HANDLE) vkDestroyImageView(device, doomed.sampled, nullptr);
        if (doomed.top != VK_NULL_HANDLE) vkDestroyImageView(device, doomed.top, nullptr);
        destroy_vma_image(allocator, doomed.image, doomed.memory);
    });
    image = {};
}

void WaterFftPass::retire_images() {
    displacement_textures_ = {};
    derivatives_texture_.reset();
    for (auto* set : {&spectra_a_, &spectra_b_, &displacement_}) {
        for (auto& image : *set) retire_image(image);
    }
    retire_image(derivatives_);
    for (auto& buffer : amplitude_buffers_) {
        if (buffer) gpu::vulkan::retire(*gpu_device_, std::move(buffer));
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
        if (auto r = create_image(spectra_a_[i], SPECTRUM_FORMAT, resolution_, 1, SPECTRUM_USAGE,
                                  "water_fft_spectra_a"); !r) return r;
        if (auto r = create_image(spectra_b_[i], SPECTRUM_FORMAT, resolution_, 1, SPECTRUM_USAGE,
                                  "water_fft_spectra_b"); !r) return r;
        if (auto r = create_image(displacement_[i], SURFACE_FORMAT, resolution_, mips_, SURFACE_USAGE,
                                  "water_fft_displacement", &displacement_textures_[i]); !r) {
            return r;
        }
    }
    if (auto r = create_image(derivatives_, SURFACE_FORMAT, resolution_, mips_, SURFACE_USAGE,
                              "water_fft_derivatives", &derivatives_texture_); !r) return r;

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

void WaterFftPass::set_sea(const FjWaterData::SimulatedSea* sea) {
    if (verifying_ && (sea == nullptr || sea != &*verify_sea_)) return;   // not replaced under it

    if (sea == nullptr || sea->spectrum == nullptr) {
        spectrum_.reset();
        return;
    }
    patch_size_ = sea->drawn_from.patch_size;
    choppiness_ = sea->choppiness;
    if (sea->spectrum == spectrum_) return;

    const std::uint32_t resolution = sea->spectrum->resolution;
    if (resolution < MIN_RESOLUTION || resolution > MAX_RESOLUTION || patch_size_ <= 0.0f) {
        if (spectrum_ != nullptr || resolution_ != resolution) {
            FJELL_CORE_WARN("WaterFftPass: a simulated sea needs a patch with a size and a "
                            "resolution from {} to {}, got {} m at {}", MIN_RESOLUTION,
                            MAX_RESOLUTION, patch_size_, resolution);
        }
        resolution_ = resolution;   // so the warning is said once per offending sea
        images_ready_ = false;
        spectrum_.reset();
        return;
    }

    if (resolution != resolution_ || !images_ready_) {
        retire_images();
        resolution_ = resolution;
        if (auto built = build_images(); !built) {
            FJELL_CORE_ERROR("{}", built.error());
            retire_images();
            spectrum_.reset();
            return;
        }
    }
    spectrum_ = sea->spectrum;
    k_spacing_ = spectrum_->k_spacing;
    amplitudes_stale_.fill(true);
}

void WaterFftPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    stepped_this_frame_ = false;
    time_ = verifying_ ? VERIFY_TIME : ctx.elapsed_time;

    if (verifying_ && verify_frames_left_ > 0 && --verify_frames_left_ == 0) {
        finish_verification();
    }
}

PassSetup WaterFftPass::setup() const {
    return {
        .name = "water_fft",
        .phase = RenderPhase::Environment,
        .sub_order = 65,   // a slot of its own, after the cloud shadow; nothing here reads it
    };
}

void WaterFftPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    // Steps once a frame, in the first run that keeps it; with no sea running
    // the still texel is all there is and nothing is written.
    if (stepped_this_frame_ || !active()) { return; }
    // The graph moves both surfaces into GENERAL for the transform's write;
    // the step's own mip chain, blitted, leaves them at rest.
    for (const char* name : {"water_displacement", "water_derivatives"}) {
        auto image = builder.import_named(ctx, name);
        builder.write(image, gpu::Access::storage_write_compute);
        builder.leaves(image, gpu::Access::copy_dst, SURFACE_READS);
    }
    // New images: last frame's surface is cleared to still water, and the
    // graph returns it to rest.
    if (images_fresh_) {
        builder.write(builder.import_named(ctx, "water_displacement_prev"), gpu::Access::copy_dst);
    }
}

void WaterFftPass::collect_exports(DeclareContext& ctx) {
    // Every surface rests where its readers sample it. Fresh images hold
    // nothing until the first step; the still texel is seeded when made.
    auto add = [&](std::string_view name, gpu::Texture texture, bool unwritten) {
        ImportedImage img{};
        img.view = texture;
        img.persistent = true;   // this frame's surface is the next frame's previous one
        img.resting = SURFACE_READS;
        img.unwritten = unwritten;
        ctx.imports.emplace(fg_name_hash(name), img);
    };
    if (!active()) {
        if (still_.image == VK_NULL_HANDLE) { return; }
        for (const char* name : {"water_displacement", "water_displacement_prev",
                                 "water_derivatives"}) {
            add(name, still_texture_, false);
        }
        return;
    }
    const std::uint32_t written = written_index();
    add("water_displacement", displacement_textures_[written], images_fresh_);
    add("water_displacement_prev", displacement_textures_[1 - written], images_fresh_);
    add("water_derivatives", derivatives_texture_, images_fresh_);
}

Result<> WaterFftPass::seed_still(const PassResources& resources) {
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = resources.command_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    vk_check(vkAllocateCommandBuffers(device_, &alloc, &cmd),
             "Failed to allocate the still water seed command buffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin), "Failed to begin the still water seed");

    auto to_clear = mip_barrier(still_.image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    submit_barriers(cmd, &to_clear, 1);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, still_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                         &whole);
    auto to_read = mip_barrier(still_.image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    submit_barriers(cmd, &to_read, 1);

    vk_check(vkEndCommandBuffer(cmd), "Failed to end the still water seed");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk_check(vkQueueSubmit(resources.graphics_queue, 1, &submit, VK_NULL_HANDLE),
             "Failed to submit the still water seed");
    vk_check(vkQueueWaitIdle(resources.graphics_queue), "Failed to wait for the still water seed");
    vkFreeCommandBuffers(device_, resources.command_pool, 1, &cmd);
    return {};
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
    to_read.dstStageMask = SURFACE_READERS;
    submit_barriers(cmd, &to_read, 1);
}

void WaterFftPass::record(const FrameContext& ctx) {
    if (stepped_this_frame_) return;
    stepped_this_frame_ = true;

    VkCommandBuffer cmd = ctx.cmd;
    if (!active()) return;

    FJELL_PROFILE_SCOPE_N("record_water_fft");
    FJELL_GPU_ZONE(ctx, "WaterFft");

    // This step writes the surface that was the previous one. Swapped here,
    // with the work, rather than when the frame is prepared: a frame that is
    // prepared and never recorded would otherwise leave "this frame's" surface
    // naming the older of the two.
    cur_ = 1 - cur_;
    const std::uint32_t slot = ctx.frame_index % MAX_FRAMES_IN_FLIGHT;
    // Written here rather than when the sea was set: a sea can be set after
    // this pass has prepared its frame, and the transform must never run from
    // a buffer that has not been filled.
    if (amplitudes_stale_[slot]) {
        amplitude_buffers_[slot]->write(spectrum_->amplitudes.data(),
                                        spectrum_->amplitudes.size() * sizeof(glm::vec2));
        amplitudes_stale_[slot] = false;
    }
    const std::uint32_t groups = (resolution_ + 7) / 8;
    const auto log2_resolution = static_cast<std::uint32_t>(std::countr_zero(resolution_));

    // The spectra are written whole, so what they held before is of no
    // interest and they start from an undefined layout. The graph has the
    // surface and its derivatives in GENERAL already.
    std::array<VkImageMemoryBarrier2, 4> to_write{};
    std::uint32_t barrier_count = 0;
    for (std::uint32_t i = 0; i < 2; ++i) {
        to_write[barrier_count++] = mip_barrier(spectra_a_[i].image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL, 0,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        to_write[barrier_count++] = mip_barrier(spectra_b_[i].image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL, 0,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    submit_barriers(cmd, to_write.data(), barrier_count);

    if (images_fresh_) {
        // Nothing has been simulated into last frame's surface yet. Still
        // water is the honest thing for it to hold: the surface then appears
        // to have risen out of a flat sea over one frame, not out of whatever
        // was in the memory. The graph has it in TRANSFER_DST_OPTIMAL.
        const Image& previous = displacement_[1 - cur_];
        const VkClearColorValue still{};
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, mips_, 0, 1};
        vkCmdClearColorImage(cmd, previous.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &still, 1,
                             &all);
        images_fresh_ = false;
    }

    // Turn the drawn sea to now.
    {
        FJELL_PROFILE_SCOPE_N("water_fft_evolve");
        FJELL_GPU_ZONE(ctx, "WaterFft Evolve");
        FrameCacheBinding b[3]{};
        b[0].binding = 0;
        b[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[0].buffer.buffer = amplitude_buffers_[slot]->handle();
        b[0].buffer.range = VK_WHOLE_SIZE;
        b[1] = storage_image(1, spectra_a_[0].top);
        b[2] = storage_image(2, spectra_b_[0].top);
        VkDescriptorSet set = ctx.descriptor_cache->acquire(evolve_.set_layout, {b, 3});

        const EvolvePush push{resolution_, log2_resolution, k_spacing_, time_, choppiness_};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, evolve_.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, evolve_.layout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(cmd, evolve_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, groups, groups, 1);
    }

    // Transform: every stage along x, then every stage along z.
    std::uint32_t source = 0;
    {
        FJELL_PROFILE_SCOPE_N("water_fft_butterflies");
        FJELL_GPU_ZONE(ctx, "WaterFft Butterflies");
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
                VkDescriptorSet set =
                    ctx.descriptor_cache->acquire(butterfly_.set_layout, {b, 4});

                const ButterflyPush push{resolution_, stage, vertical};
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, butterfly_.layout,
                                        0, 1, &set, 0, nullptr);
                vkCmdPushConstants(cmd, butterfly_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(push), &push);
                vkCmdDispatch(cmd, groups, groups, 1);
                source = 1 - source;
            }
        }
    }

    // Gather the surface and how it changes across the patch.
    {
        FJELL_PROFILE_SCOPE_N("water_fft_assemble");
        FJELL_GPU_ZONE(ctx, "WaterFft Assemble");
        compute_to_compute(cmd);
        const FrameCacheBinding b[4]{
            storage_image(0, spectra_a_[source].top),
            storage_image(1, spectra_b_[source].top),
            storage_image(2, displacement_[cur_].top),
            storage_image(3, derivatives_.top),
        };
        VkDescriptorSet set = ctx.descriptor_cache->acquire(assemble_.set_layout, {b, 4});

        const AssemblePush push{resolution_, patch_size_ / static_cast<float>(resolution_)};
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

    {
        FJELL_PROFILE_SCOPE_N("water_fft_mips");
        FJELL_GPU_ZONE(ctx, "WaterFft Mips");
        generate_mips(cmd, displacement_[cur_]);
        generate_mips(cmd, derivatives_);
    }
}

void WaterFftPass::begin_verification() {
    // Every setting spelled out: this is a fixed sea to hold the transform to,
    // and it should not move when a contributor's defaults do.
    FftContributor sea;
    sea.wind_direction = {0.8f, 0.6f};
    sea.wind_speed = 9.0f;
    sea.fetch = 100000.0f;
    sea.spread_degrees = 35.0f;
    sea.high_cut = 0.0f;
    sea.amplitude_scale = 1.0f;
    sea.patch_size = 96.0f;
    // Small enough that the plain Fourier sum it is held to takes a moment
    // rather than minutes, large enough to run every kind of butterfly stage.
    sea.resolution = 64;
    sea.choppiness = 1.2f;
    sea.seed = 7;
    verify_sea_ = water::draw_simulated_sea(sea);
    verifying_ = true;
    set_sea(&*verify_sea_);
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
    const water::DisplacementSpectra spectra =
        water::evolve_spectrum(*spectrum_, choppiness_, VERIFY_TIME);
    const std::vector<float> height = water::inverse_transform_reference(spectra.height, n);
    const std::vector<float> along_x = water::inverse_transform_reference(spectra.along_x, n);
    const std::vector<float> along_z = water::inverse_transform_reference(spectra.along_z, n);

    const auto* read = static_cast<const std::uint16_t*>(verify_readback_->mapped());
    const std::size_t texels = static_cast<std::size_t>(n) * n;
    const float across = 1.0f / (2.0f * patch_size_ / static_cast<float>(n));
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
