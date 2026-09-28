#pragma once

#include "core/result.hpp"
#include "gpu/binding.hpp"
#include "gpu/buffer.hpp"
#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/sampler.hpp"
#include "gpu/shader.hpp"
#include "gpu/texture.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace fjell::gpu {

/// What the device can do, where it differs between GPUs.
struct Caps {
    /// The most vertices and primitives a mesh shader workgroup may output.
    uint32_t mesh_max_output_vertices{0};
    uint32_t mesh_max_output_primitives{0};
    /// Bytes of push data a pipeline may take: 128 on every GPU the engine
    /// runs on, which is what every pipeline keeps to.
    uint32_t max_push_size{128};
};

/// Finds a file the build produced (`"shaders/grid.vert.spv"`) and returns its
/// path, for a program whose files live somewhere of its own.
using ShaderLocator = std::function<std::string(const std::string& relative)>;

/// The GPU: what makes buffers, textures, samplers and pipelines and destroys
/// them once the GPU is done with them.
///
/// Creating and releasing belong to one thread, the one that submits frames;
/// any thread may look up what exists (views and samplers included).
///
/// The backend's state lives in `Impl`, which only the backend defines. The
/// Vulkan backend makes a device with `vulkan::create_device()`.
class Device {
public:
    struct Impl;

    explicit Device(std::unique_ptr<Impl> impl);
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    /// A buffer as described.
    /// @return the buffer, or the reason it could not be made (the message
    ///         names it).
    [[nodiscard]] Result<Owned<Buffer>> create(const BufferDesc& desc);

    /// A texture as described, holding `desc.initial` before the next frame
    /// when one is given.
    /// @return the texture, or the reason it could not be made (the message
    ///         names it).
    [[nodiscard]] Result<Owned<Texture>> create(const TextureDesc& desc);

    /// The sampler for `desc`: the same handle every time for the same
    /// description, valid as long as the device.
    [[nodiscard]] Sampler sampler(const SamplerDesc& desc);

    /// The CPU's view of a buffer in `Memory::upload` or `Memory::readback`;
    /// empty for `Memory::gpu` and for a handle that finds no buffer.
    [[nodiscard]] std::span<std::byte> mapped(Buffer buffer) const;

    /// A buffer's size in bytes; 0 for a handle that finds no buffer.
    [[nodiscard]] uint64_t size(Buffer buffer) const;

    /// What a texture was made as; an empty `TextureInfo` (0 × 0, undefined
    /// format) for a handle that finds no texture.
    [[nodiscard]] const TextureInfo& info(Texture texture) const;

    /// A compute pipeline, its layout read from the shader.
    /// @return the pipeline, or why it could not be made (the message names it
    ///         and the shader).
    [[nodiscard]] Result<Owned<ComputePipeline>> create(const ComputePipelineDesc& desc);

    /// A graphics pipeline, its layout read from the shaders. A mesh shader
    /// declaring more output than `caps()` allows is refused.
    /// @return the pipeline, or why it could not be made (the message names it
    ///         and the shaders).
    [[nodiscard]] Result<Owned<GraphicsPipeline>> create(const GraphicsPipelineDesc& desc);

    /// Rebuilds a pipeline from `desc` in place, for hot reload: the handle
    /// stays valid and the old pipeline is released once the GPU is done
    /// with it.
    /// @return nothing, or why the new one could not be made, in which case
    ///         the old one stays.
    [[nodiscard]] Result<> recreate(ComputePipeline pipeline, const ComputePipelineDesc& desc);
    [[nodiscard]] Result<> recreate(GraphicsPipeline pipeline, const GraphicsPipelineDesc& desc);

    /// What a pipeline's shaders bind and take; an empty layout for a handle
    /// that finds no pipeline.
    [[nodiscard]] const ShaderLayout& layout(ComputePipeline pipeline) const;
    [[nodiscard]] const ShaderLayout& layout(GraphicsPipeline pipeline) const;

    /// A persistent bind group: the entries fill one of the pipeline's own
    /// sets, checked against what its shaders declare. A set a shared layout
    /// takes is bound through the engine's shared group instead.
    /// @return the group, or which entry or binding is wrong (named).
    [[nodiscard]] Result<Owned<BindGroup>> create(const BindGroupDesc& desc);

    /// Fills a persistent group anew, safe at any time: commands recorded
    /// after it bind the new resources, those recorded before keep the old.
    /// @return nothing, or which entry is wrong, in which case the group keeps
    ///         what it held.
    [[nodiscard]] Result<> update(BindGroup group, std::span<const BindEntry> entries);

    /// How shaders named by path are found. Without one, a path is opened as
    /// given, relative to the working directory.
    void set_shader_locator(ShaderLocator locator);

    [[nodiscard]] const Caps& caps() const;

    /// The backend's state, for the backend's own code.
    [[nodiscard]] Impl& impl() noexcept { return *impl_; }
    [[nodiscard]] const Impl& impl() const noexcept { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace fjell::gpu
