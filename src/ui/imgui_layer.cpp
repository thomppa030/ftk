#include "ui/imgui_layer.hpp"
#include "ui/editor_view_settings.hpp"
#include "ui/theme.hpp"
#include "renderer/gpu/window.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <SDL3/SDL.h>

#include "core/log.hpp"
#include "core/profiler.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace fjell {

namespace {

std::vector<uint32_t> read_spirv_words(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) return {};
  auto size = static_cast<size_t>(file.tellg());
  std::vector<uint32_t> words(size / sizeof(uint32_t));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(words.data()),
            static_cast<std::streamsize>(words.size() * sizeof(uint32_t)));
  return words;
}

} // namespace

ImGuiLayer::ImGuiLayer(Window& window, VkInstance instance,
                       VkPhysicalDevice physical_device, VkDevice device,
                       uint32_t graphics_family, VkQueue graphics_queue,
                       VkFormat color_format, uint32_t frames_in_flight,
                       const ImGuiLayerFiles& files)
    : window_{window}, device_{device} {
  // Descriptor pool for ImGui. Every ImGui::Image texture holds one set for
  // as long as it is registered: editor icons, a directory's worth of
  // texture thumbnails, the resident asset thumbnails, viewport images.
  // When this runs out AddTexture returns null and the image silently does
  // not draw, so the size leaves generous room above those counts.
  constexpr uint32_t MAX_IMAGE_DESCRIPTORS = 4096;
  std::array<VkDescriptorPoolSize, 1> pool_sizes = {{
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_IMAGE_DESCRIPTORS},
  }};

  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool_info.maxSets = MAX_IMAGE_DESCRIPTORS;
  pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
  pool_info.pPoolSizes = pool_sizes.data();

  if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) !=
      VK_SUCCESS) {
    throw std::runtime_error("Failed to create ImGui descriptor pool");
  }

  IMGUI_CHECKVERSION();
  auto* prev_ctx = ImGui::GetCurrentContext();
  context_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(context_);

  // Before the first frame reads the ini the section is saved in.
  register_editor_view_settings();

  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  setup_style();
  theme::load_font(files.fonts.string());

  ImGui_ImplSDL3_InitForVulkan(window.handle());
  // SDL has one cursor for the whole program, and the backend sets it only
  // when its own context's choice changes, which leaves one window's cursor
  // over the next. It keeps its hands off, and update_cursor() shows this
  // window's choice only while the mouse is over it.
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  cursors_.resize(ImGuiMouseCursor_COUNT, nullptr);
  cursors_[ImGuiMouseCursor_Arrow] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
  cursors_[ImGuiMouseCursor_TextInput] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
  cursors_[ImGuiMouseCursor_ResizeAll] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
  cursors_[ImGuiMouseCursor_ResizeNS] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
  cursors_[ImGuiMouseCursor_ResizeEW] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
  cursors_[ImGuiMouseCursor_ResizeNESW] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NESW_RESIZE);
  cursors_[ImGuiMouseCursor_ResizeNWSE] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NWSE_RESIZE);
  cursors_[ImGuiMouseCursor_Hand] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
  cursors_[ImGuiMouseCursor_Wait] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAIT);
  cursors_[ImGuiMouseCursor_Progress] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_PROGRESS);
  cursors_[ImGuiMouseCursor_NotAllowed] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NOT_ALLOWED);

  ImGui_ImplVulkan_InitInfo init_info{};
  init_info.Instance = instance;
  init_info.PhysicalDevice = physical_device;
  init_info.Device = device;
  init_info.QueueFamily = graphics_family;
  init_info.Queue = graphics_queue;
  init_info.DescriptorPool = descriptor_pool_;
  // The backend swallows Vulkan failures unless told where to report them;
  // an exhausted descriptor pool would otherwise show up only as images
  // that stop drawing.
  init_info.CheckVkResultFn = [](VkResult result) {
    if (result != VK_SUCCESS) {
      FJELL_GFX_ERROR("ImGui Vulkan backend: VkResult={}", static_cast<int>(result));
    }
  };
  // The backend's "image count" is how many sets of vertex buffers it keeps
  // and uses in turn, one per frame: enough for every frame in flight. The
  // minimum is only for swapchains of its own, which it is never asked for.
  init_info.MinImageCount = 2;
  init_info.ImageCount = std::max(frames_in_flight, init_info.MinImageCount);
  init_info.UseDynamicRendering = true;
  init_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
  init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
  init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

  frag_spv_ = read_spirv_words(files.srgb_fragment.string());
  if (frag_spv_.empty()) {
    // The stock stage writes ImGui's sRGB colours as if they were linear,
    // so every swatch and style colour comes out one gamma too bright.
    FJELL_CORE_ERROR("ImGui: {} not found; colours will render too bright",
                     files.srgb_fragment.string());
  } else {
    init_info.CustomShaderFragCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    init_info.CustomShaderFragCreateInfo.codeSize = frag_spv_.size() * sizeof(uint32_t);
    init_info.CustomShaderFragCreateInfo.pCode = frag_spv_.data();
  }

  ImGui_ImplVulkan_Init(&init_info);

  // The window hands over only its own events, so each context sees its
  // own window's input whichever context is current when they arrive.
  event_connection_ = window.on_event.bind([this](const SDL_Event& event) {
    ImGuiContext* current = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(context_);
    ImGui_ImplSDL3_ProcessEvent(&event);
    ImGui::SetCurrentContext(current);
  });

  // Restore the previously active context so creating a second ImGuiLayer
  // (e.g. for the import dialog) doesn't hijack the editor's context.
  // If there was no previous context (first ImGuiLayer), keep ours active.
  if (prev_ctx) {
    ImGui::SetCurrentContext(prev_ctx);
  }
}

Delegate<void(void*)> ImGuiLayer::on_context_destroyed;

ImGuiLayer::~ImGuiLayer() {
  event_connection_.disconnect();

  auto* prev = ImGui::GetCurrentContext();
  if (prev == context_) prev = nullptr; // don't restore ourselves

  ImGui::SetCurrentContext(context_);
  // Announce while the context is still current, so listeners can inspect it.
  on_context_destroyed.broadcast(static_cast<void*>(context_));
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  theme::forget_fonts(context_);
  ImGui::DestroyContext(context_);
  context_ = nullptr;

  // Restore the previous context so the editor keeps working
  ImGui::SetCurrentContext(prev);

  if (descriptor_pool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
  }
  for (SDL_Cursor* cursor : cursors_) {
    SDL_DestroyCursor(cursor);
  }
}

void ImGuiLayer::activate() {
  prev_context_ = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(context_);
}

void ImGuiLayer::deactivate() {
  ImGui::SetCurrentContext(prev_context_);
  prev_context_ = nullptr;
}

void ImGuiLayer::begin_frame() {
  FJELL_PROFILE_SCOPE_N("imgui_begin_frame");
  if (!context_) {
    FJELL_CORE_ERROR("ImGuiLayer::begin_frame() called with no ImGui context");
    return;
  }

  ImGui::SetCurrentContext(context_);
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
}

void ImGuiLayer::end_frame() {
  FJELL_PROFILE_SCOPE_N("imgui_end_frame");
  ImGui::Render();
  update_cursor();
}

void ImGuiLayer::update_cursor() {
  // Another window has the mouse, or the game has it captured and hidden.
  SDL_Window* window = window_.handle();
  if (SDL_GetMouseFocus() != window || SDL_GetWindowRelativeMouseMode(window)) {
    return;
  }

  const ImGuiMouseCursor shape = ImGui::GetMouseCursor();
  if (ImGui::GetIO().MouseDrawCursor || shape < 0
      || static_cast<size_t>(shape) >= cursors_.size()) {
    SDL_HideCursor();
    return;
  }
  SDL_Cursor* wanted = cursors_[static_cast<size_t>(shape)];
  if (wanted == nullptr) wanted = cursors_[ImGuiMouseCursor_Arrow];
  // SDL redraws on every set, so only when the shape changes.
  if (SDL_GetCursor() != wanted) SDL_SetCursor(wanted);
  SDL_ShowCursor();
}

void ImGuiLayer::render(VkCommandBuffer cmd) {
  FJELL_PROFILE_SCOPE_N("imgui_render");
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void ImGuiLayer::setup_style() {
  theme::apply(ImGui::GetStyle());
}

} // namespace fjell
