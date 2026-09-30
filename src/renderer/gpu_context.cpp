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
    if (auto made = meshlet_descriptor_.create(core_.gpu_device()); !made) {
        throw std::runtime_error("Meshlet set: " + made.error());
    }
    shared_layouts_.meshlets = meshlet_descriptor_.layout();

    // Four samples where the GPU has them.
    msaa_samples_ = std::min(gpu::Samples::x4, core_.gpu_device().caps().max_samples);
    FJELL_GFX_INFO("MSAA: {}x", static_cast<uint32_t>(msaa_samples_));


    FJELL_GFX_INFO("GPU context ready");
}

void GpuContext::init_pipelines() {
    pipeline_cache_ = std::make_unique<PipelineCache>(
        core_.gpu_device(),
        PipelineCache::Targets{
            .color = OffscreenPass::COLOR_FORMAT,
            .mr = OffscreenPass::MR_FORMAT,
            .normal = OffscreenPass::NORMAL_FORMAT,
            .velocity = OffscreenPass::VELOCITY_FORMAT,
            .depth = OffscreenPass::DEPTH_FORMAT,
            .samples = msaa_samples_,
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

    for (auto& buffer : tile_light_ssbo_) buffer.reset();
    view_data_fallback_.reset();
    unoccluded_.reset();
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

    // Tile light index SSBO: max tiles, each with count + MAX_LIGHTS_PER_TILE indices
    constexpr uint64_t tile_ssbo_size = MAX_TILES * sizeof(uint32_t) +
                                        MAX_TILES * MAX_LIGHTS_PER_TILE * sizeof(uint32_t);

    auto make = [&](uint64_t size, gpu::BufferUses use, gpu::Memory memory, std::string_view name) {
        auto made = device.create(gpu::BufferDesc{.size = size, .use = use, .memory = memory, .name = name});
        if (!made) throw std::runtime_error(made.error());
        return std::move(*made);
    };
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        // Light cull writes it, maybe on the async compute queue, and the
        // fragment shaders read it on graphics.
        tile_light_ssbo_[i] = make(tile_ssbo_size, gpu::BufferUse::storage, gpu::Memory::gpu, "tile_light_ssbo");
    }
    const uint64_t view_data_size = std::max({uint64_t{sizeof(GlobalUbo)}, uint64_t{sizeof(EnvironmentUbo)},
                                              LIGHT_SSBO_SIZE, uint64_t{MAX_FOG_VOLUMES} * sizeof(GpuFogVolume)});
    view_data_fallback_ = make(view_data_size, gpu::BufferUse::uniform | gpu::BufferUse::storage,
                               gpu::Memory::upload, "view_data_fallback");
    std::ranges::fill(device.mapped(view_data_fallback_), std::byte{0});

    depth_sampler_ = device.sampler({.filter = gpu::Filter::nearest,
                                     .mip_filter = gpu::Filter::nearest,
                                     .address = gpu::Address::clamp});
    gi_fallback_sampler_ = device.sampler({.address = gpu::Address::clamp});

    // Cleared to zero before anything samples them. A reader multiplies what
    // it samples by a strength that is zero when the real image is missing,
    // and zero times whatever garbage new memory holds is not zero when that
    // garbage is a NaN.
    auto fallback = [&](gpu::TextureKind kind, gpu::Format format, float value, std::string_view name) {
        auto made = device.create(gpu::TextureDesc{
            .kind = kind,
            .format = format,
            .use = gpu::TextureUse::sampled,
            .queues = gpu::Queues::graphics_and_compute,
            .initial = gpu::Clear{value, value, value, value},
            .name = name,
        });
        if (!made) throw std::runtime_error(made.error());
        return std::move(*made);
    };
    gi_fallback_ = fallback(gpu::TextureKind::tex2d, gpu::Format::rgba16_float, 0.0f, "gi_fallback");
    volume_fallback_ = fallback(gpu::TextureKind::tex3d, gpu::Format::r16_float, 0.0f, "volume_fallback");
    unoccluded_ = fallback(gpu::TextureKind::tex2d, gpu::Format::r8_unorm, 1.0f, "unoccluded");

    FJELL_GFX_DEBUG("Descriptor resources created (layout pending finalize)");
}

void GpuContext::finalize_descriptors() {
    if (auto made = global_bindings_.create(core_.gpu_device()); !made) {
        throw std::runtime_error("Global set: " + made.error());
    }

    // Write initial descriptor values for static bindings
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        global_bindings_.set(i, "tile_light_ssbo", gpu::storage(tile_light_ssbo_[i]));
        global_bindings_.set(i, "fog_density_field", gpu::sampled(volume_fallback_, gi_fallback_sampler_));
        reset_view_bindings(i);
    }

    shared_layouts_.globals = global_bindings_.layout();

    FJELL_GFX_DEBUG("Global descriptors finalized");
}

void GpuContext::reset_view_bindings(uint32_t slot) {
    const gpu::BindResource black = gpu::sampled(gi_fallback_, gi_fallback_sampler_);
    const gpu::BindResource unoccluded = gpu::sampled(unoccluded_, gi_fallback_sampler_);
    const gpu::BindEntry entries[] = {
        {"global_ubo", gpu::uniform(gpu::BufferRange(view_data_fallback_, 0, sizeof(GlobalUbo)))},
        {"environment_ubo", gpu::uniform(gpu::BufferRange(view_data_fallback_, 0, sizeof(EnvironmentUbo)))},
        {"light_ssbo", gpu::storage(gpu::BufferRange(view_data_fallback_, 0, LIGHT_SSBO_SIZE))},
        {"fog_volume_ssbo",
         gpu::storage(gpu::BufferRange(view_data_fallback_, 0, uint64_t{MAX_FOG_VOLUMES} * sizeof(GpuFogVolume)))},
        {"depth_texture", black},
        {"ao_texture", unoccluded},
        {"screen_color", black},
        {"gi_indirect", black},
        {"ss_gi_emissive", black},
        {"contact_shadow", unoccluded},
    };
    global_bindings_.set(slot, entries);
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

void GpuContext::share_draw_data_layout(gpu::SharedLayout layout) {
    shared_layouts_.draw_data = layout;
}

PassResources GpuContext::pass_resources() const {
    PassResources res{};
    res.globals = shared_layouts_.globals;
    res.bindless = shared_layouts_.bindless;
    res.draw_data = shared_layouts_.draw_data;
    res.meshlets = shared_layouts_.meshlets;
    res.scene_color_format = OffscreenPass::COLOR_FORMAT;
    res.scene_depth_format = OffscreenPass::DEPTH_FORMAT;
    res.scene_mr_format = OffscreenPass::MR_FORMAT;
    res.scene_normal_format = OffscreenPass::NORMAL_FORMAT;
    res.scene_velocity_format = OffscreenPass::VELOCITY_FORMAT;
    res.scene_distortion_format = OffscreenPass::DISTORTION_FORMAT;
    res.samples = msaa_samples_;
    // Handing out mutable access to the device core_ owns, not mutating
    // core_ itself — same "logically const" shape as every other pointer/
    // handle field above, just needing a cast since gpu_device() isn't
    // const-qualified on GpuCore (its callers call non-const methods on it).
    auto& mutable_core = const_cast<GpuCore&>(core_);
    res.device = &mutable_core.gpu_device();
    return res;
}

} // namespace fjell
