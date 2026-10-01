#include "ftk/app/imgui_layer.hpp"
#include "ftk/ui/theme.hpp"
#include "ftk/gpu/window.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include "ftk/gpu/imgui_renderer.hpp"

#include <SDL3/SDL.h>

#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace ftk {

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

ImGuiLayer::ImGuiLayer(Window& window, gpu::Device& device, gpu::Format color_format,
                       const ImGuiLayerFiles& files)
    : window_{window} {
  IMGUI_CHECKVERSION();
  auto* prev_ctx = ImGui::GetCurrentContext();
  context_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(context_);

  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  setup_style();
  if (!theme::load_font(files.fonts.string())) {
    FTK_CORE_ERROR("ImGui: the theme's fonts are not in {}; text falls back to ImGui's own",
                   files.fonts.string());
  }

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

  // Decodes ImGui's sRGB colours for the sRGB target (shaders/imgui.frag).
  const std::vector<uint32_t> srgb_fragment = read_spirv_words(files.srgb_fragment.string());
  if (srgb_fragment.empty()) {
    // The stock stage writes ImGui's sRGB colours as if they were linear,
    // so every swatch and style colour comes out one gamma too bright.
    FTK_CORE_ERROR("ImGui: {} not found; colours will render too bright",
                     files.srgb_fragment.string());
  }
  renderer_ = std::make_unique<gpu::ImGuiRenderer>(
      device, gpu::ImGuiRenderer::Desc{.target_format = color_format, .fragment = srgb_fragment});
  io.UserData = this;

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

ImGuiLayer::~ImGuiLayer() {
  event_connection_.disconnect();

  auto* prev = ImGui::GetCurrentContext();
  if (prev == context_) prev = nullptr; // don't restore ourselves

  ImGui::SetCurrentContext(context_);
  renderer_.reset();
  ImGui_ImplSDL3_Shutdown();
  theme::forget_fonts(context_);
  ImGui::DestroyContext(context_);
  context_ = nullptr;

  // Restore the previous context so the editor keeps working
  ImGui::SetCurrentContext(prev);

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
  FTK_PROFILE_SCOPE_N("imgui_begin_frame");
  if (!context_) {
    FTK_CORE_ERROR("ImGuiLayer::begin_frame() called with no ImGui context");
    return;
  }

  ImGui::SetCurrentContext(context_);
  renderer_->new_frame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
}

void ImGuiLayer::end_frame() {
  FTK_PROFILE_SCOPE_N("imgui_end_frame");
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

void ImGuiLayer::render(gpu::RenderEncoder& pass) {
  renderer_->render(pass);
}

ImTextureID ImGuiLayer::texture(const gpu::TextureView& view, gpu::Sampler sampler) {
  // The backend makes the image in the current context's pool, which has
  // to be this layer's for the image to go back to it.
  ImGuiContext* current = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(context_);
  const ImTextureID id = renderer_->texture(view, sampler);
  ImGui::SetCurrentContext(current);
  return id;
}

ImTextureID ImGuiLayer::texture(const gpu::TextureView& view) {
  ImGuiContext* current = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(context_);
  const ImTextureID id = renderer_->texture(view);
  ImGui::SetCurrentContext(current);
  return id;
}

ImGuiLayer* ImGuiLayer::current() {
  if (ImGui::GetCurrentContext() == nullptr) return nullptr;
  return static_cast<ImGuiLayer*>(ImGui::GetIO().UserData);
}

void ImGuiLayer::setup_style() {
  theme::apply(ImGui::GetStyle());
}

ImTextureID imgui_texture(const gpu::TextureView& view, gpu::Sampler sampler) {
  ImGuiLayer* layer = ImGuiLayer::current();
  return layer != nullptr ? layer->texture(view, sampler) : ImTextureID{};
}

ImTextureID imgui_texture(const gpu::TextureView& view) {
  ImGuiLayer* layer = ImGuiLayer::current();
  return layer != nullptr ? layer->texture(view) : ImTextureID{};
}

} // namespace ftk
