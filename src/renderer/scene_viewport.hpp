#pragma once

#include "renderer/offscreen_pass.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/viewport_source.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>

namespace fjell {

class BindlessTextures;
class Device;

/// Configuration for creating a SceneViewport.
struct SceneViewportConfig {
    std::string name;
    uint32_t width{512};
    uint32_t height{512};
    PostProcessConfig pp_config{};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    bool expose_hdr{false};    // true = HDR color (R16F), false = tonemapped LDR (sRGB)
    bool skip_shadows{true};
};

/// Camera/view parameters for a SceneViewport.
struct SceneViewportView {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 camera_position{0.0f};
};

/// A named render target that renders a scene and exposes its output
/// as a bindless texture index, usable by any material.
class SceneViewport {
public:
    SceneViewport(Device& device, VkDevice vk_device, VmaAllocator allocator,
                  const SceneViewportConfig& config,
                  const PostProcessResources::PassRefs& pass_refs,
                  uint32_t reserved_bindless_index);
    ~SceneViewport();

    SceneViewport(const SceneViewport&) = delete;
    SceneViewport& operator=(const SceneViewport&) = delete;
    SceneViewport(SceneViewport&&) = delete;
    SceneViewport& operator=(SceneViewport&&) = delete;

    /// Resize the viewport. Must be called in the safe window (after fence wait).
    void resize(uint32_t width, uint32_t height);

    /// Update the bindless entry to point to the current output view.
    /// Call after rendering completes.
    void sync_bindless(BindlessTextures& bindless);

    /// Build a ViewportRenderRequest for this viewport.
    [[nodiscard]] ViewportRenderRequest build_request() const;

    /// The image view to sample in materials (HDR or tonemapped based on config).
    [[nodiscard]] VkImageView output_view() const;

    void set_view(const SceneViewportView& view) { view_ = view; }

    [[nodiscard]] const std::string& name() const { return config_.name; }
    [[nodiscard]] uint32_t bindless_index() const { return bindless_index_; }
    [[nodiscard]] OffscreenPass& offscreen_pass() { return *offscreen_; }
    [[nodiscard]] PostProcessResources& pp_resources() { return *pp_; }
    [[nodiscard]] const SceneViewportConfig& config() const { return config_; }
    [[nodiscard]] const SceneViewportView& view() const { return view_; }

private:
    SceneViewportConfig config_;
    SceneViewportView view_;
    uint32_t bindless_index_;

    std::unique_ptr<OffscreenPass> offscreen_;
    std::unique_ptr<PostProcessResources> pp_;

    VkSampler output_sampler_{VK_NULL_HANDLE};
    VkDevice vk_device_{VK_NULL_HANDLE};
    VkImageView last_synced_view_{VK_NULL_HANDLE};
};

} // namespace fjell
