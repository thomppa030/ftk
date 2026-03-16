#include "ui/imgui_layer.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <ImGuizmo.h>

#include <array>
#include <stdexcept>

namespace fjell {

ImGuiLayer::ImGuiLayer(GLFWwindow* window, VkInstance instance,
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

    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create ImGui descriptor pool");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
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
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
}

void ImGuiLayer::end_frame() {
    ImGui::Render();
}

void ImGuiLayer::render(VkCommandBuffer cmd) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void ImGuiLayer::setup_style() {
    auto& style = ImGui::GetStyle();
    auto& colors = style.Colors;

    // Rounding
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.PopupRounding = 4.0f;

    // Spacing
    style.WindowPadding = {10.0f, 10.0f};
    style.FramePadding = {8.0f, 4.0f};
    style.ItemSpacing = {8.0f, 6.0f};
    style.ItemInnerSpacing = {6.0f, 4.0f};
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;

    // Dark theme with blue-grey accents
    colors[ImGuiCol_WindowBg] = {0.08f, 0.08f, 0.10f, 0.94f};
    colors[ImGuiCol_ChildBg] = {0.10f, 0.10f, 0.12f, 1.00f};
    colors[ImGuiCol_Border] = {0.20f, 0.22f, 0.27f, 0.60f};

    colors[ImGuiCol_FrameBg] = {0.14f, 0.15f, 0.18f, 1.00f};
    colors[ImGuiCol_FrameBgHovered] = {0.20f, 0.22f, 0.27f, 1.00f};
    colors[ImGuiCol_FrameBgActive] = {0.24f, 0.26f, 0.33f, 1.00f};

    colors[ImGuiCol_TitleBg] = {0.06f, 0.06f, 0.08f, 1.00f};
    colors[ImGuiCol_TitleBgActive] = {0.10f, 0.12f, 0.16f, 1.00f};
    colors[ImGuiCol_TitleBgCollapsed] = {0.04f, 0.04f, 0.06f, 0.60f};

    colors[ImGuiCol_Header] = {0.16f, 0.18f, 0.24f, 1.00f};
    colors[ImGuiCol_HeaderHovered] = {0.22f, 0.25f, 0.33f, 1.00f};
    colors[ImGuiCol_HeaderActive] = {0.26f, 0.30f, 0.40f, 1.00f};

    colors[ImGuiCol_Button] = {0.18f, 0.20f, 0.26f, 1.00f};
    colors[ImGuiCol_ButtonHovered] = {0.26f, 0.30f, 0.40f, 1.00f};
    colors[ImGuiCol_ButtonActive] = {0.32f, 0.36f, 0.48f, 1.00f};

    colors[ImGuiCol_Tab] = {0.12f, 0.13f, 0.17f, 1.00f};
    colors[ImGuiCol_TabHovered] = {0.26f, 0.30f, 0.40f, 1.00f};
    colors[ImGuiCol_TabSelected] = {0.18f, 0.20f, 0.28f, 1.00f};

    colors[ImGuiCol_Separator] = {0.20f, 0.22f, 0.27f, 0.50f};
    colors[ImGuiCol_SeparatorHovered] = {0.36f, 0.44f, 0.60f, 0.78f};
    colors[ImGuiCol_SeparatorActive] = {0.44f, 0.52f, 0.70f, 1.00f};

    colors[ImGuiCol_SliderGrab] = {0.36f, 0.42f, 0.56f, 1.00f};
    colors[ImGuiCol_SliderGrabActive] = {0.44f, 0.52f, 0.70f, 1.00f};

    colors[ImGuiCol_ScrollbarBg] = {0.06f, 0.06f, 0.08f, 0.40f};
    colors[ImGuiCol_ScrollbarGrab] = {0.22f, 0.24f, 0.30f, 1.00f};
    colors[ImGuiCol_ScrollbarGrabHovered] = {0.30f, 0.33f, 0.40f, 1.00f};
    colors[ImGuiCol_ScrollbarGrabActive] = {0.36f, 0.40f, 0.50f, 1.00f};

    colors[ImGuiCol_CheckMark] = {0.50f, 0.60f, 0.82f, 1.00f};
    colors[ImGuiCol_TextSelectedBg] = {0.24f, 0.30f, 0.44f, 0.50f};
    colors[ImGuiCol_PlotHistogram] = {0.50f, 0.60f, 0.82f, 1.00f};

    colors[ImGuiCol_Text] = {0.86f, 0.88f, 0.92f, 1.00f};
    colors[ImGuiCol_TextDisabled] = {0.42f, 0.44f, 0.48f, 1.00f};
}

} // namespace fjell
