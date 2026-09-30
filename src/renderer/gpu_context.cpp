#include "renderer/gpu_context.hpp"
#include "gpu/vulkan/native.hpp"
#include "gpu/vulkan/translate.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/uniform_types.hpp"
#include "environment/environment_ubo.hpp"
#include "core/log.hpp"
#include "renderer/gpu/window.hpp"

#include "core/base_path.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>

namespace fjell {

// ── Pipeline cache persistence ──────────────────────────────────────────

static VkPipelineCache load_pipeline_cache(VkDevice device) {
    VkPipelineCache cache = VK_NULL_HANDLE;
    std::vector<char> data;
    auto path = base_path::resolve("pipeline_cache.bin");
    if (std::filesystem::exists(path)) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (f.is_open()) {
            auto file_size = static_cast<size_t>(f.tellg());
            data.resize(file_size);
            f.seekg(0);
            f.read(data.data(), static_cast<std::streamsize>(file_size));
            FJELL_GFX_INFO("Pipeline cache loaded from disk ({} bytes)", file_size);
        }
    }

    VkPipelineCacheCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();

    if (vkCreatePipelineCache(device, &ci, nullptr, &cache) != VK_SUCCESS) {
        FJELL_GFX_WARN("Failed to create pipeline cache, starting fresh");
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        vkCreatePipelineCache(device, &ci, nullptr, &cache);
    }
    return cache;
}

static void save_pipeline_cache(VkDevice device, VkPipelineCache cache) {
    if (cache == VK_NULL_HANDLE) return;

    size_t size = 0;
    vkGetPipelineCacheData(device, cache, &size, nullptr);
    std::vector<char> data(size);
    vkGetPipelineCacheData(device, cache, &size, data.data());

    auto path = base_path::resolve("pipeline_cache.bin");
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f.is_open()) {
        f.write(data.data(), static_cast<std::streamsize>(size));
        FJELL_GFX_INFO("Pipeline cache saved to disk ({} bytes)", size);
    }
}

// ── GpuContext ───────────────────────────────────────────────────────────

GpuContext::GpuContext(Window& window)
    : core_{window}
    , render_stats_{std::make_unique<RenderStats>()} {
    FJELL_GFX_INFO("Initializing GPU context");

    auto swapchain = gpu::Swapchain::create(core_.gpu_device(), window);
    if (!swapchain) throw std::runtime_error(swapchain.error());
    swapchain_ = std::move(*swapchain);

    vk_pipeline_cache_ = load_pipeline_cache(core_.vk_device());
    // The GPU interface's pipelines read the shaders the build compiled beside
    // the program, through the engine's pipeline cache.
    core_.gpu_device().set_shader_locator(
        [](const std::string& relative) { return base_path::resolve(relative); });
    gpu::vulkan::use_pipeline_cache(core_.gpu_device(), vk_pipeline_cache_);

    create_descriptor_resources();
    if (auto r = bindless_textures_.create(core_.gpu_device()); !r) {
        throw std::runtime_error(r.error());
    }
    shared_layouts_.bindless = bindless_textures_.layout();

    transient_pool_.create(core_.gpu_device());

    // Multi-draw indirect setup
    mega_buffer_ = std::make_unique<MegaBuffer>(core_.gpu_device());

    // Meshlet buffers + descriptor (uploaded at model load time)
    meshlet_buffer_ = std::make_unique<MeshletBuffer>(core_.gpu_device());
    meshlet_descriptor_.create(core_.vk_device(), core_.gpu_device(), core_.allocator());

    // Query and clamp MSAA sample count
    auto max_samples = core_.device().max_msaa_samples();
    msaa_samples_ = static_cast<VkSampleCountFlagBits>(
        std::min(static_cast<uint32_t>(VK_SAMPLE_COUNT_4_BIT),
                 static_cast<uint32_t>(max_samples)));
    FJELL_GFX_INFO("MSAA: {}x", static_cast<uint32_t>(msaa_samples_));


    FJELL_GFX_INFO("GPU context ready");
}

void GpuContext::init_pipelines() {
    VkFormat depth_format = core_.device().find_depth_format();

    pipeline_cache_ = std::make_unique<PipelineCache>(
        core_.gpu_device(),
        PipelineCache::Targets{
            .color = gpu::vulkan::from_vk(OffscreenPass::COLOR_FORMAT),
            .mr = gpu::vulkan::from_vk(OffscreenPass::MR_FORMAT),
            .normal = gpu::vulkan::from_vk(OffscreenPass::NORMAL_FORMAT),
            .velocity = gpu::vulkan::from_vk(OffscreenPass::VELOCITY_FORMAT),
            .depth = gpu::vulkan::from_vk(depth_format),
            .samples = gpu::vulkan::from_vk(msaa_samples_),
        },
        PipelineCache::SharedLayouts{
            .globals = shared_layouts_.globals,
            .bindless = shared_layouts_.bindless,
            .draw_data = shared_layouts_.draw_data,
            .meshlets = core_.device().mesh_shader_supported() ? shared_layouts_.meshlets
                                                                : gpu::SharedLayout{},
        });
}

GpuContext::~GpuContext() {
    auto vk_device = core_.vk_device();

    save_pipeline_cache(vk_device, vk_pipeline_cache_);
    gpu::vulkan::use_pipeline_cache(core_.gpu_device(), VK_NULL_HANDLE);
    if (vk_pipeline_cache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(vk_device, vk_pipeline_cache_, nullptr);
        vk_pipeline_cache_ = VK_NULL_HANDLE;
    }

    for (auto* buffers : {&uniform_buffers_, &environment_buffers_, &light_ssbo_, &tile_light_ssbo_,
                          &fog_volume_ssbo_}) {
        for (auto& buffer : *buffers) buffer.reset();
    }
    volume_fallback_.reset();
    gi_fallback_.reset();

    meshlet_descriptor_.destroy();
    meshlet_buffer_.reset();
    mega_buffer_.reset();

    transient_pool_.destroy();
    global_bindings_.destroy();
    bindless_textures_.destroy();
    pipeline_cache_.reset();


    secondary_pools_.destroy();

    // The swapchain goes next, once the GPU is idle, then GpuCore: the
    // command pool, the device, the allocator, in that order.
}

void GpuContext::create_descriptor_resources() {
    // GlobalBindings constructor already registered the builtin bindings.
    // Buffers, samplers, and fallback images are created here.
    // Layout/pool/set creation happens in finalize_descriptors() to allow
    // additional bindings to be declared before the layout is locked.
    gpu::Device& device = core_.gpu_device();

    // Light SSBO: 1024 point + 1024 spot lights (~48 KB)
    constexpr uint64_t light_ssbo_size = 1024 * sizeof(GpuPointLight) + 1024 * sizeof(GpuSpotLight);
    // Tile light index SSBO: max tiles, each with count + MAX_LIGHTS_PER_TILE indices
    constexpr uint64_t tile_ssbo_size = MAX_TILES * sizeof(uint32_t) +
                                        MAX_TILES * MAX_LIGHTS_PER_TILE * sizeof(uint32_t);
    // Fog volume SSBO: the scene's local fog volumes (~7 KB)
    constexpr uint64_t fog_volume_ssbo_size = MAX_FOG_VOLUMES * sizeof(GpuFogVolume);

    auto make = [&](uint64_t size, gpu::BufferUse use, gpu::Memory memory, std::string_view name) {
        auto made = device.create(gpu::BufferDesc{.size = size, .use = use, .memory = memory, .name = name});
        if (!made) throw std::runtime_error(made.error());
        return std::move(*made);
    };
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        uniform_buffers_[i] = make(sizeof(GlobalUbo), gpu::BufferUse::uniform, gpu::Memory::upload, "global_ubo");
        environment_buffers_[i] =
            make(sizeof(EnvironmentUbo), gpu::BufferUse::uniform, gpu::Memory::upload, "environment_ubo");
        light_ssbo_[i] = make(light_ssbo_size, gpu::BufferUse::storage, gpu::Memory::upload, "light_ssbo");
        // Light cull writes it, maybe on the async compute queue, and the
        // fragment shaders read it on graphics.
        tile_light_ssbo_[i] = make(tile_ssbo_size, gpu::BufferUse::storage, gpu::Memory::gpu, "tile_light_ssbo");
        fog_volume_ssbo_[i] =
            make(fog_volume_ssbo_size, gpu::BufferUse::storage, gpu::Memory::upload, "fog_volume_ssbo");
    }

    depth_sampler_ = device.sampler({.filter = gpu::Filter::nearest,
                                     .mip_filter = gpu::Filter::nearest,
                                     .address = gpu::Address::clamp});
    gi_fallback_sampler_ = device.sampler({.address = gpu::Address::clamp});

    // Cleared to zero before anything samples them. A reader multiplies what
    // it samples by a strength that is zero when the real image is missing,
    // and zero times whatever garbage new memory holds is not zero when that
    // garbage is a NaN.
    auto fallback = [&](gpu::TextureKind kind, gpu::Format format, std::string_view name) {
        auto made = device.create(gpu::TextureDesc{
            .kind = kind,
            .format = format,
            .use = gpu::TextureUse::sampled,
            .queues = gpu::Queues::graphics_and_compute,
            .initial = gpu::Clear{0.0f, 0.0f, 0.0f, 0.0f},
            .name = name,
        });
        if (!made) throw std::runtime_error(made.error());
        return std::move(*made);
    };
    gi_fallback_ = fallback(gpu::TextureKind::tex2d, gpu::Format::rgba16_float, "gi_fallback");
    volume_fallback_ = fallback(gpu::TextureKind::tex3d, gpu::Format::r16_float, "volume_fallback");

    FJELL_GFX_DEBUG("Descriptor resources created (layout pending finalize)");
}

gpu::BindGroup GpuContext::adopted_group(AdoptedGroups& groups, gpu::SharedLayout layout,
                                         VkDescriptorSet set) {
    auto& group = groups[set];
    if (!group) group = gpu::vulkan::adopt_group(core_.gpu_device(), layout, set);
    return group;
}

gpu::BindGroup GpuContext::global_group(VkDescriptorSet set) {
    return adopted_group(global_groups_, shared_layouts_.globals, set);
}

gpu::BindGroup GpuContext::draw_data_group(VkDescriptorSet set) {
    return adopted_group(draw_data_groups_, shared_layouts_.draw_data, set);
}

gpu::BindGroup GpuContext::meshlets_group(VkDescriptorSet set) {
    return adopted_group(meshlets_groups_, shared_layouts_.meshlets, set);
}

void GpuContext::finalize_descriptors() {
    auto vk_device = core_.vk_device();

    // Build layout, pool, and per-frame descriptor sets from registered bindings
    global_bindings_.finalize(vk_device, MAX_FRAMES_IN_FLIGHT, core_.gpu_device());

    // Write initial descriptor values for static bindings
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        global_bindings_.set(i, "global_ubo", gpu::uniform(uniform_buffers_[i]));
        global_bindings_.set(i, "environment_ubo", gpu::uniform(environment_buffers_[i]));
        global_bindings_.set(i, "light_ssbo", gpu::storage(light_ssbo_[i]));
        global_bindings_.set(i, "tile_light_ssbo", gpu::storage(tile_light_ssbo_[i]));
        global_bindings_.set(i, "fog_volume_ssbo", gpu::storage(fog_volume_ssbo_[i]));
        global_bindings_.set(i, "gi_indirect", gpu::sampled(gi_fallback_, gi_fallback_sampler_));
        global_bindings_.set(i, "ss_gi_emissive", gpu::sampled(gi_fallback_, gi_fallback_sampler_));
        global_bindings_.set(i, "fog_density_field", gpu::sampled(volume_fallback_, gi_fallback_sampler_));
    }

    // The GPU interface's pipelines name these to bind the engine's own sets.
    gpu::Device& device = core_.gpu_device();
    shared_layouts_.globals = gpu::vulkan::share_layout(
        device, "globals", 0, global_bindings_.layout(), global_bindings_.layout_bindings());
    shared_layouts_.meshlets = gpu::vulkan::share_layout(
        device, "meshlets", 3, meshlet_descriptor_.layout(), meshlet_descriptor_.layout_bindings());

    FJELL_GFX_DEBUG("Global descriptors finalized");
}

void GpuContext::ensure_secondary_pools(uint32_t thread_count) {
    if (!secondary_pools_initialized_ && thread_count > 0) {
        secondary_pools_.create(core_.vk_device(), core_.graphics_family(),
                                thread_count + 1);
        secondary_pools_initialized_ = true;
    }
}

FrameDescriptorCache& GpuContext::descriptor_cache() {
    return gpu::vulkan::frame_cache(core_.gpu_device());
}

const FrameDescriptorCache& GpuContext::descriptor_cache() const {
    return gpu::vulkan::frame_cache(core_.gpu_device());
}

void GpuContext::share_draw_data_layout(VkDescriptorSetLayout layout,
                                        std::span<const VkDescriptorSetLayoutBinding> bindings) {
    shared_layouts_.draw_data =
        gpu::vulkan::share_layout(core_.gpu_device(), "draw data", 2, layout, bindings);
}

PassResources GpuContext::pass_resources() const {
    PassResources res{};
    res.globals = shared_layouts_.globals;
    res.bindless = shared_layouts_.bindless;
    res.draw_data = shared_layouts_.draw_data;
    res.meshlets = shared_layouts_.meshlets;
    res.global_set_layout = global_bindings_.layout();
    res.material_set_layout = gpu::vulkan::native_set_layout(core_.gpu_device(), bindless_textures_.layout());
    res.draw_data_layout = VK_NULL_HANDLE; // set by caller if needed
    res.meshlet_layout = core_.device().mesh_shader_supported() ? meshlet_descriptor_.layout() : VK_NULL_HANDLE;
    res.depth_format = core_.device().find_depth_format();
    res.color_format = OffscreenPass::COLOR_FORMAT;
    res.sample_count = msaa_samples_;
    res.scene_color_format = gpu::vulkan::from_vk(res.color_format);
    res.scene_depth_format = gpu::vulkan::from_vk(res.depth_format);
    res.scene_mr_format = gpu::vulkan::from_vk(OffscreenPass::MR_FORMAT);
    res.scene_normal_format = gpu::vulkan::from_vk(OffscreenPass::NORMAL_FORMAT);
    res.scene_velocity_format = gpu::vulkan::from_vk(OffscreenPass::VELOCITY_FORMAT);
    res.samples = gpu::vulkan::from_vk(res.sample_count);
    res.allocator = core_.allocator();
    res.pipeline_cache = vk_pipeline_cache_;
    res.command_pool = core_.command_pool();
    res.graphics_queue = core_.graphics_queue();
    // Handing out mutable access to objects core_ owns, not mutating
    // core_ itself — same "logically const" shape as every other pointer/
    // handle field above, just needing a cast since these two accessors
    // aren't const-qualified on GpuCore (they hand out mutable state on
    // purpose; upload_context()/gpu_device() callers need to call
    // non-const methods on them).
    auto& mutable_core = const_cast<GpuCore&>(core_);
    res.upload_context = &mutable_core.upload_context();
    res.device = &mutable_core.gpu_device();
    return res;
}

} // namespace fjell
