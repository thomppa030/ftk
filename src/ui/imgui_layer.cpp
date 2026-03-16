#include "ui/imgui_layer.hpp"

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
                       VkRenderPass render_pass, uint32_t image_count)
    : device_{device} {
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
  auto &style = ImGui::GetStyle();
  auto &c = style.Colors;

  // Discord palette
  // Blurple: #5865F2  Blurple hover: #4752C4  Green: #57F287
  // Dark:    #1e1f22  Mid: #2b2d31  Light: #313338  Input: #1e1f22
  constexpr ImVec4 blurple = {0.345f, 0.396f, 0.949f, 1.0f};
  constexpr ImVec4 blurple_dim = {0.345f, 0.396f, 0.949f, 0.40f};
  constexpr ImVec4 blurple_hov = {0.278f, 0.322f, 0.769f, 1.0f};
  constexpr ImVec4 green = {0.341f, 0.949f, 0.529f, 1.0f};

  // Geometry
  style.WindowRounding = 0.0f;
  style.FrameRounding = 4.0f;
  style.GrabRounding = 2.0f;
  style.ScrollbarRounding = 8.0f;
  style.TabRounding = 2.0f;
  style.ChildRounding = 0.0f;
  style.PopupRounding = 4.0f;

  style.WindowPadding = {10.0f, 8.0f};
  style.FramePadding = {8.0f, 4.0f};
  style.ItemSpacing = {8.0f, 4.0f};
  style.ItemInnerSpacing = {4.0f, 4.0f};
  style.IndentSpacing = 16.0f;
  style.ScrollbarSize = 10.0f;
  style.GrabMinSize = 8.0f;
  style.WindowBorderSize = 0.0f;
  style.FrameBorderSize = 0.0f;
  style.TabBorderSize = 0.0f;

  // #1e1f22 = 0.118, 0.122, 0.133
  // #2b2d31 = 0.169, 0.176, 0.192
  // #313338 = 0.192, 0.200, 0.220
  // #383a40 = 0.220, 0.227, 0.251
  // #404249 = 0.251, 0.259, 0.286
  // #4e5058 = 0.306, 0.314, 0.345

  // -- Backgrounds --
  c[ImGuiCol_WindowBg] = {0.118f, 0.122f, 0.133f, 1.0f}; // #1e1f22
  c[ImGuiCol_ChildBg] = {0.118f, 0.122f, 0.133f, 1.0f};  // same as WindowBg
  c[ImGuiCol_PopupBg] = {0.067f, 0.071f, 0.078f, 0.98f}; // #111214
  c[ImGuiCol_Border] = {0.055f, 0.055f, 0.063f, 1.0f};
  c[ImGuiCol_BorderShadow] = {0.0f, 0.0f, 0.0f, 0.0f};

  // -- Frames (inputs recessed darker) --
  c[ImGuiCol_FrameBg] = {0.067f, 0.071f, 0.078f, 1.0f};        // #111214
  c[ImGuiCol_FrameBgHovered] = {0.118f, 0.122f, 0.133f, 1.0f}; // #1e1f22
  c[ImGuiCol_FrameBgActive] = {0.169f, 0.176f, 0.192f, 1.0f};  // #2b2d31

  // -- Title / menu --
  c[ImGuiCol_TitleBg] = {0.067f, 0.071f, 0.078f, 1.0f}; // #111214
  c[ImGuiCol_TitleBgActive] = {0.067f, 0.071f, 0.078f, 1.0f};
  c[ImGuiCol_TitleBgCollapsed] = {0.067f, 0.071f, 0.078f, 0.6f};
  c[ImGuiCol_MenuBarBg] = {0.067f, 0.071f, 0.078f, 1.0f};

  // -- Headers / selectables --
  c[ImGuiCol_Header] = {0.169f, 0.176f, 0.192f, 1.0f};        // #2b2d31
  c[ImGuiCol_HeaderHovered] = {0.192f, 0.200f, 0.220f, 1.0f}; // #313338
  c[ImGuiCol_HeaderActive] = blurple_dim;

  // -- Buttons (blurple) --
  c[ImGuiCol_Button] = {0.169f, 0.176f, 0.192f, 1.0f}; // #2b2d31
  c[ImGuiCol_ButtonHovered] = blurple;
  c[ImGuiCol_ButtonActive] = blurple_hov;

  // -- Tabs --
  c[ImGuiCol_Tab] = {0.067f, 0.071f, 0.078f, 1.0f};         // #111214
  c[ImGuiCol_TabHovered] = {0.169f, 0.176f, 0.192f, 1.0f};  // #2b2d31
  c[ImGuiCol_TabSelected] = {0.118f, 0.122f, 0.133f, 1.0f}; // #1e1f22
  c[ImGuiCol_TabDimmed] = {0.067f, 0.071f, 0.078f, 1.0f};
  c[ImGuiCol_TabDimmedSelected] = {0.090f, 0.094f, 0.106f, 1.0f};

  // -- Separators --
  c[ImGuiCol_Separator] = {0.055f, 0.055f, 0.063f, 1.0f};
  c[ImGuiCol_SeparatorHovered] = blurple_dim;
  c[ImGuiCol_SeparatorActive] = blurple;

  // -- Resize grip --
  c[ImGuiCol_ResizeGrip] = {0.0f, 0.0f, 0.0f, 0.0f};
  c[ImGuiCol_ResizeGripHovered] = blurple_dim;
  c[ImGuiCol_ResizeGripActive] = blurple;

  // -- Scrollbar --
  c[ImGuiCol_ScrollbarBg] = {0.067f, 0.071f, 0.078f, 0.5f};
  c[ImGuiCol_ScrollbarGrab] = {0.169f, 0.176f, 0.192f, 1.0f};
  c[ImGuiCol_ScrollbarGrabHovered] = {0.220f, 0.227f, 0.251f, 1.0f};
  c[ImGuiCol_ScrollbarGrabActive] = {0.306f, 0.314f, 0.345f, 1.0f};

  // -- Slider --
  c[ImGuiCol_SliderGrab] = blurple;
  c[ImGuiCol_SliderGrabActive] = blurple_hov;

  // -- Check / radio --
  c[ImGuiCol_CheckMark] = green;

  // -- Plot --
  c[ImGuiCol_PlotLines] = blurple;
  c[ImGuiCol_PlotLinesHovered] = green;
  c[ImGuiCol_PlotHistogram] = blurple;

  // -- Docking --
  c[ImGuiCol_DockingPreview] = blurple_dim;
  c[ImGuiCol_DockingEmptyBg] = {0.067f, 0.071f, 0.078f, 1.0f};

  // -- Text --
  c[ImGuiCol_Text] = {0.898f, 0.902f, 0.918f, 1.0f};         // #e5e7ea
  c[ImGuiCol_TextDisabled] = {0.447f, 0.455f, 0.486f, 1.0f}; // #72747c
  c[ImGuiCol_TextSelectedBg] = blurple_dim;

  // -- Nav / tables --
  c[ImGuiCol_NavHighlight] = blurple;
  c[ImGuiCol_DragDropTarget] = green;
  c[ImGuiCol_TableHeaderBg] = {0.067f, 0.071f, 0.078f, 1.0f};
  c[ImGuiCol_TableBorderStrong] = {0.055f, 0.055f, 0.063f, 1.0f};
  c[ImGuiCol_TableBorderLight] = {0.055f, 0.055f, 0.063f, 0.5f};
  c[ImGuiCol_TableRowBg] = {0.0f, 0.0f, 0.0f, 0.0f};
  c[ImGuiCol_TableRowBgAlt] = {0.118f, 0.122f, 0.133f, 0.15f};
}

} // namespace fjell
