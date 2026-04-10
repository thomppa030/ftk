#include "ui/imgui_layer.hpp"
#include "ui/theme.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <ImGuizmo.h>

#include "core/log.hpp"
#include "core/profiler.hpp"

#include <array>
#include <stdexcept>

namespace fjell {

ImGuiLayer::ImGuiLayer(GLFWwindow *window, VkInstance instance,
                       VkPhysicalDevice physical_device, VkDevice device,
                       uint32_t graphics_family, VkQueue graphics_queue,
                       VkFormat color_format, uint32_t image_count,
                       const std::string& font_dir)
    : device_{device}, font_dir_{font_dir} {
  // Descriptor pool for ImGui
  std::array<VkDescriptorPoolSize, 1> pool_sizes = {{
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100},
  }};

  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool_info.maxSets = 100;
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

  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  setup_style();
  theme::load_font(font_dir_);

  ImGui_ImplGlfw_InitForVulkan(window, true);

  ImGui_ImplVulkan_InitInfo init_info{};
  init_info.Instance = instance;
  init_info.PhysicalDevice = physical_device;
  init_info.Device = device;
  init_info.QueueFamily = graphics_family;
  init_info.Queue = graphics_queue;
  init_info.DescriptorPool = descriptor_pool_;
  init_info.MinImageCount = 2;
  init_info.ImageCount = image_count;
  init_info.UseDynamicRendering = true;
  init_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
  init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
  init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

  ImGui_ImplVulkan_Init(&init_info);

  // Restore the previously active context so creating a second ImGuiLayer
  // (e.g. for the import dialog) doesn't hijack the editor's context.
  // If there was no previous context (first ImGuiLayer), keep ours active.
  if (prev_ctx) {
    ImGui::SetCurrentContext(prev_ctx);
  }
}

ImGuiLayer::~ImGuiLayer() {
  auto* prev = ImGui::GetCurrentContext();
  if (prev == context_) prev = nullptr; // don't restore ourselves

  ImGui::SetCurrentContext(context_);
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext(context_);
  context_ = nullptr;

  // Restore the previous context so the editor keeps working
  ImGui::SetCurrentContext(prev);

  if (descriptor_pool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
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
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
  ImGuizmo::BeginFrame();
}

void ImGuiLayer::end_frame() { FJELL_PROFILE_SCOPE_N("imgui_end_frame"); ImGui::Render(); }

void ImGuiLayer::render(VkCommandBuffer cmd) {
  FJELL_PROFILE_SCOPE_N("imgui_render");
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void ImGuiLayer::setup_style() {
  theme::apply(ImGui::GetStyle());
}

} // namespace fjell
