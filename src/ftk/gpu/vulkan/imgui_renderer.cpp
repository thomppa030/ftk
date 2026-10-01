#include "ftk/gpu/imgui_renderer.hpp"

#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/render_encoder.hpp"
#include "ftk/gpu/vulkan/command_list_impl.hpp"
#include "ftk/gpu/vulkan/device_impl.hpp"
#include "ftk/gpu/vulkan/native.hpp"
#include "ftk/gpu/vulkan/translate.hpp"

#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace fjell::gpu {

/// The list a scope records into, for the ImGui backend, which records
/// through Vulkan itself.
struct RenderEncoderBackend {
    static CommandList& list(RenderEncoder& pass) { return *pass.list_; }
};

namespace {

// A texture as ImGui shows it: the view and how it is sampled.
struct ImageKey {
    TextureView view;
    Sampler sampler;
    bool operator==(const ImageKey&) const = default;
};

struct ImageKeyHash {
    size_t operator()(const ImageKey& key) const noexcept {
        size_t h = std::hash<uint64_t>{}((uint64_t{key.view.texture.id} << 32) | key.sampler.id);
        auto mix = [&](uint32_t value) { h ^= std::hash<uint32_t>{}(value) + 0x9e3779b9 + (h << 6) + (h >> 2); };
        mix(key.view.base_mip);
        mix(key.view.mip_count);
        mix(key.view.base_layer);
        mix(key.view.layer_count);
        mix(static_cast<uint32_t>(key.view.kind));
        mix(static_cast<uint32_t>(key.view.format));
        return h;
    }
};

} // namespace

struct ImGuiRenderer::State {
    VkDevice device{VK_NULL_HANDLE};
    /// Every image's set comes from here, and goes back here when its
    /// texture is released; destroying it frees whatever is left.
    VkDescriptorPool pool{VK_NULL_HANDLE};
    /// The backend keeps a pointer to the stage for its lifetime.
    std::vector<uint32_t> fragment;
    std::unordered_map<ImageKey, VkDescriptorSet, ImageKeyHash> images;
};

ImGuiRenderer::ImGuiRenderer(Device& device, const Desc& desc)
    : device_(device),
      linear_(device.sampler({.filter = Filter::linear, .address = Address::clamp})),
      state_(std::make_shared<State>()) {
    Device::Impl& impl = device.impl();
    State& state = *state_;
    state.device = impl.device;

    // Every image shown holds a set for as long as its texture lives:
    // editor icons, a folder's thumbnails, the resident asset thumbnails,
    // viewport images. Run out and an image silently stops drawing, so the
    // size leaves generous room above those counts.
    constexpr uint32_t MAX_IMAGES = 4096;
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_IMAGES};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = MAX_IMAGES;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &size;
    if (vkCreateDescriptorPool(state.device, &pool_info, nullptr, &state.pool) != VK_SUCCESS) {
        throw std::runtime_error("ImGui: the descriptor pool could not be made");
    }

    ImGui_ImplVulkan_InitInfo init{};
    init.Instance = impl.foundation.instance();
    init.PhysicalDevice = impl.foundation.physical_device();
    init.Device = impl.device;
    init.QueueFamily = impl.families[0];
    init.Queue = impl.queues[0];
    init.DescriptorPool = state.pool;
    // The backend swallows Vulkan failures unless told where to report them;
    // an exhausted pool would otherwise show up only as images that stop
    // drawing.
    init.CheckVkResultFn = [](VkResult result) {
        if (result != VK_SUCCESS) FJELL_GFX_ERROR("ImGui Vulkan backend: VkResult={}", static_cast<int>(result));
    };
    // The backend's "image count" is how many sets of vertex buffers it keeps
    // and uses in turn, one per frame: enough for every frame in flight. The
    // minimum is only for swapchains of its own, which it is never asked for.
    init.MinImageCount = 2;
    init.ImageCount = std::max(device.caps().frames_in_flight, init.MinImageCount);
    init.UseDynamicRendering = true;
    const VkFormat format = vulkan::to_vk(desc.target_format);
    init.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!desc.fragment.empty()) {
        state.fragment.assign(desc.fragment.begin(), desc.fragment.end());
        init.CustomShaderFragCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        init.CustomShaderFragCreateInfo.codeSize = state.fragment.size() * sizeof(uint32_t);
        init.CustomShaderFragCreateInfo.pCode = state.fragment.data();
    }
    ImGui_ImplVulkan_Init(&init);
}

ImGuiRenderer::~ImGuiRenderer() {
    // The frames in flight draw with the backend's buffers and the pool's sets.
    device_.wait_idle();
    ImGui_ImplVulkan_Shutdown();
    // Frees every image's set with it; the textures that would have let
    // theirs go find the state gone.
    vkDestroyDescriptorPool(state_->device, state_->pool, nullptr);
    state_->images.clear();
}

void ImGuiRenderer::new_frame() {
    ImGui_ImplVulkan_NewFrame();
}

void ImGuiRenderer::render(RenderEncoder& pass) {
    FJELL_PROFILE_SCOPE_N("imgui_render");
    CommandList& list = RenderEncoderBackend::list(pass);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vulkan::native_command_buffer(list));
    // The backend bound a pipeline and sets of its own behind the list's
    // back; nothing the list remembers binding still holds.
    list.impl().pipeline = nullptr;
    list.impl().bound_sets = 0;
}

ImTextureID ImGuiRenderer::texture(const TextureView& view, Sampler sampler) {
    State& state = *state_;
    const ImageKey key{view, sampler};
    if (auto it = state.images.find(key); it != state.images.end()) {
        return reinterpret_cast<ImTextureID>(it->second);
    }

    Device::Impl& impl = device_.impl();
    auto* record = impl.textures.get(view.texture);
    if (record == nullptr) return ImTextureID{};
    const FormatKind texels = kind(record->info.format);
    const VkImageLayout layout = texels == FormatKind::depth || texels == FormatKind::depth_stencil
        ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
        vulkan::native_sampler(device_, sampler), vulkan::native_view(device_, view), layout);
    if (set == VK_NULL_HANDLE) return ImTextureID{};
    state.images.emplace(key, set);

    // When the texture goes, so does its image here, unless the renderer
    // went first and took the pool with it.
    std::lock_guard lock(impl.views_mutex);
    record->on_release.emplace_back([weak = std::weak_ptr<State>(state_), key] {
        const auto alive = weak.lock();
        if (!alive) return;
        const auto it = alive->images.find(key);
        if (it == alive->images.end()) return;
        vkFreeDescriptorSets(alive->device, alive->pool, 1, &it->second);
        alive->images.erase(it);
    });
    return reinterpret_cast<ImTextureID>(set);
}

} // namespace fjell::gpu
