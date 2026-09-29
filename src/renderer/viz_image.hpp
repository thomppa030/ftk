#pragma once

#include "gpu/owned.hpp"
#include "gpu/render_encoder.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace fjell {

class PassBuilder;
struct DeclareContext;

// The picture a visualisation pass draws for a viewport to show in place of
// the scene: a single-sample colour texture the editor displays through
// ImGui. The pass exports it to the frame graph under its name and declares
// its draw into it, and the graph leaves it where ImGui samples it.
class VizImage {
public:
    VizImage() = default;
    ~VizImage();

    VizImage(const VizImage&) = delete;
    VizImage& operator=(const VizImage&) = delete;
    VizImage(VizImage&&) = delete;
    VizImage& operator=(VizImage&&) = delete;

    /// A 64 × 64 image in `format` until the first resize, known to the graph
    /// as `name`.
    void init(gpu::Device& device, gpu::Format format, std::string name);
    void cleanup();

    /// Made again at `width` × `height` when that changes; the frames still
    /// reading the old one keep it until they are done.
    void resize(uint32_t width, uint32_t height);

    /// Exports the texture to the graph, resting where ImGui samples it.
    void collect_exports(DeclareContext& ctx) const;
    /// Declares the draw into the texture. False, declaring nothing, when
    /// there is no texture to draw into.
    bool declare(PassBuilder& builder, const DeclareContext& ctx) const;
    /// The texture as the one colour target, cleared to `clear`.
    [[nodiscard]] gpu::RenderTargets targets(const gpu::Clear& clear) const;
    /// The texture as the one colour target, what it held discarded: for a
    /// draw that covers every pixel.
    [[nodiscard]] gpu::RenderTargets targets() const;

    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();

    /// Mark that the image has been drawn into since it was made.
    void mark_rendered() { has_rendered_ = true; }

    [[nodiscard]] gpu::Texture texture() const { return texture_; }
    [[nodiscard]] bool valid() const { return static_cast<bool>(texture_); }

private:
    void create(uint32_t width, uint32_t height);

    gpu::Device* device_{nullptr};
    gpu::Format format_{gpu::Format::undefined};
    std::string name_;
    gpu::Sampler sampler_;
    gpu::Owned<gpu::Texture> texture_;
    glm::uvec2 extent_{0, 0};
    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};
    bool has_rendered_{false};
};

} // namespace fjell
