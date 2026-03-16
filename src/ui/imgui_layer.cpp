#include "ui/imgui_layer.hpp"
#include "ui/theme.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <ImGuizmo.h>

#include "core/log.hpp"

#include <array>
#include <stdexcept>

namespace fjell {

ImGuiLayer::ImGuiLayer(GLFWwindow *window, VkInstance instance,
                       VkPhysicalDevice physical_device, VkDevice device,
                       uint32_t graphics_family, VkQueue graphics_queue,
                       VkRenderPass render_pass, uint32_t image_count,
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
  ImGui::CreateContext();

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
  init_info.PipelineInfoMain.RenderPass = render_pass;
  init_info.PipelineInfoMain.Subpass = 0;
  init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

  ImGui_ImplVulkan_Init(&init_info);
}

ImGuiLayer::~ImGuiLayer() {
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  if (descriptor_pool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
  }
}

void ImGuiLayer::begin_frame() {
  if (ImGui::GetCurrentContext() == nullptr) {
    FJELL_CORE_ERROR("ImGuiLayer::begin_frame() called with no ImGui context");
    return;
  }

  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
  ImGuizmo::BeginFrame();
}

void ImGuiLayer::end_frame() { ImGui::Render(); }

void ImGuiLayer::render(VkCommandBuffer cmd) {
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void ImGuiLayer::setup_style() {
  theme::apply(ImGui::GetStyle());
}

} // namespace fjell
