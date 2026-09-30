#pragma once

#include "core/result.hpp"
#include "gpu/acceleration.hpp"
#include "gpu/binding.hpp"
#include "gpu/buffer.hpp"
#include "gpu/frame.hpp"
#include "gpu/owned.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/readback.hpp"
#include "gpu/sampler.hpp"
#include "gpu/shader.hpp"
#include "gpu/texture.hpp"
#include "gpu/upload.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fjell {
class Window;
}

namespace fjell::gpu {

/// What the device can do, where it differs between GPUs.
struct Caps {
    /// Whether the GPU runs task and mesh shaders. Without them no pipeline
    /// with a task or mesh stage can be made, and the limits below are 0.
    bool mesh_shaders{false};
    /// The most vertices and primitives a mesh shader workgroup may output.
    uint32_t mesh_max_output_vertices{0};
    uint32_t mesh_max_output_primitives{0};
    /// Bytes of push data a pipeline may take: 128 on every GPU the engine
    /// runs on, which is what every pipeline keeps to.
    uint32_t max_push_size{128};
    /// Whether there is a compute queue beside the graphics one, which lists
    /// for `Queue::compute` run on.
    bool async_compute{false};
    /// How many frames may be recorded or on the GPU at once.
    uint32_t frames_in_flight{1};
    /// The most samples a colour and a depth target may both have.
    Samples max_samples{Samples::x1};
    /// Whether the GPU builds acceleration structures and traces them with
    /// ray queries. Without it none can be made.
    bool ray_queries{false};
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
/// The backend's state lives in `Impl`, which only the backend defines.
class Device {
public:
    struct Impl;

    /// The device for a GPU that can show `window`, with everything it runs
    /// on brought up. A program makes one and keeps it for as long as it
    /// draws; what it made must be released before the device goes.
    /// @return the device, or why none could be made (no GPU the engine runs
    ///         on, or the driver refused)
    [[nodiscard]] static Result<std::unique_ptr<Device>> create(Window& window);

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
    /// empty for `Memory::gpu` and for a handle that finds no buffer. Nothing
    /// needs flushing: what the CPU writes there is seen by work submitted
    /// after it.
    [[nodiscard]] std::span<std::byte> mapped(Buffer buffer) const;

    /// A buffer's size in bytes; 0 for a handle that finds no buffer.
    [[nodiscard]] uint64_t size(Buffer buffer) const;

    /// Makes at least `size` bytes of a buffer made with a `reserve` usable,
    /// keeping what it holds, and the handle with it: bindings made after
    /// this see the larger buffer. It grows by at least double, up to its
    /// reserve. In place where the device keeps the reserve for it;
    /// elsewhere by a copy, ordered after the uploads before it and before
    /// the work submitted after it, into a new buffer whose address differs
    /// (`generation()` moves). Nothing while it is already that large.
    /// @return nothing, or why it cannot grow: no buffer, no reserve, a size
    ///         past the reserve, or no memory.
    [[nodiscard]] Result<> grow(Buffer buffer, uint64_t size);

    /// How many times a buffer's address has moved, which a copy made to
    /// grow it does: a holder of the address (an acceleration structure
    /// built from the buffer) compares it with the count it was built at.
    /// 0 for a handle that finds no buffer.
    [[nodiscard]] uint32_t generation(Buffer buffer) const;

    /// What a texture was made as; an empty `TextureInfo` (0 × 0, undefined
    /// format) for a handle that finds no texture.
    [[nodiscard]] const TextureInfo& info(Texture texture) const;

    /// The bytes of GPU memory a texture takes; 0 for one made outside the
    /// device and for a handle that finds no texture.
    [[nodiscard]] uint64_t memory_size(Texture texture) const;

    /// An acceleration structure sized as described, in memory of its own.
    /// @return the structure, or why it could not be made: the GPU has no
    ///         ray queries, the description is empty, or there is no memory.
    [[nodiscard]] Result<Owned<AccelerationStructure>> create(const AccelerationStructureDesc& desc);

    /// Several structures in one allocation, as a model's submeshes are, each
    /// released on its own; the memory goes with the last of them. Fewer
    /// allocations than one each, which a GPU counts.
    /// @return the structures in the order described, or why they could not
    ///         be made (the message names the one that failed).
    [[nodiscard]] Result<std::vector<Owned<AccelerationStructure>>> create(
        std::span<const AccelerationStructureDesc> descs);

    /// Bytes one instance record takes in the buffer a top level is built
    /// from.
    [[nodiscard]] uint32_t instance_record_size() const;

    /// Writes `instances` as the records a top level is built from into
    /// `out`, `instance_record_size()` bytes each, which `out` has room for.
    /// An instance naming no structure is written as one no ray hits.
    void write_instances(std::span<std::byte> out, std::span<const AccelerationInstance> instances) const;

    /// What an instance record names `structure` by, for records a shader
    /// writes (the backend's GLSL layout); 0, which names nothing, for a
    /// handle that finds no structure.
    [[nodiscard]] uint64_t instance_reference(AccelerationStructure structure) const;

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

    /// A shared layout the device makes from its description: pipelines that
    /// name it take it at the set their shaders declare it at, and groups of
    /// it are made by `create(SharedGroupDesc)`. It lasts as long as the
    /// device.
    /// @return the layout, or which binding is wrong.
    [[nodiscard]] Result<SharedLayout> create(const SharedLayoutDesc& desc);

    /// A group of a shared layout the device made, bound wherever a pipeline
    /// names the layout.
    /// @return the group, or which entry is wrong or missing (named).
    [[nodiscard]] Result<Owned<BindGroup>> create(const SharedGroupDesc& desc);

    /// Fills a persistent group anew, safe at any time: commands recorded
    /// after it bind the new resources, those recorded before keep the old.
    /// A shared group's entries replace only what they name, and the rest
    /// stays; an entry naming what the group already holds costs nothing, and
    /// several updates between two binds of it cost one new version.
    /// Not called while another thread binds the same group.
    /// @return nothing, or which entry is wrong, in which case the group keeps
    ///         what it held.
    [[nodiscard]] Result<> update(BindGroup group, std::span<const BindEntry> entries);

    /// How shaders named by path are found. Without one, a path is opened as
    /// given, relative to the working directory.
    void set_shader_locator(ShaderLocator locator);

    /// Keeps what the device learns building pipelines in `file`: pipelines
    /// built from now on start from what earlier runs left there, and the
    /// device writes it back when it goes. Without one nothing is kept
    /// between runs. A file that does not read starts empty.
    void set_pipeline_cache_file(const std::filesystem::path& file);

    /// Starts the next frame: waits until the GPU has finished the frame
    /// that last used its slot, frees that slot's lists and one-frame
    /// memory, and destroys what the frames the GPU has finished released.
    /// What is released from now on waits for this frame.
    [[nodiscard]] Frame& begin_frame();

    /// Sends what `frame` submitted to the GPU, in order, with the frame's
    /// uploads before it; the GPU finishing the last list is the frame
    /// finished. A frame that submitted nothing is over at once. Frames end
    /// in the order they began.
    /// @return nothing, or why the GPU refused the work (the device is lost)
    [[nodiscard]] Result<> end_frame(Frame& frame);

    /// The newest frame the GPU has finished.
    [[nodiscard]] uint64_t finished_frame() const;

    /// Sends the uploads not yet sent and waits until the GPU has finished
    /// everything sent to it. What is released waits for the frames that may
    /// use it on its own; this is for teardown, and for code that has yet to
    /// rely on that.
    void wait_idle();

    /// Where data from the CPU goes into buffers and textures, landing before
    /// the next frame's work.
    [[nodiscard]] Upload& upload();

    /// Reads a box of one mip and layer of a colour texture, which must be
    /// made to be sampled, back to the CPU (see `Readback`).
    /// @return the read on its way, or why it cannot be made (the message
    ///         says what the texture lacks)
    [[nodiscard]] Result<Readback> read_back(const TextureView& view, const ReadbackDesc& desc = {});

    /// The GPU's name, as its driver reports it.
    [[nodiscard]] const std::string& name() const;

    [[nodiscard]] const Caps& caps() const;

    /// The backend's state, for the backend's own code.
    [[nodiscard]] Impl& impl() noexcept { return *impl_; }
    [[nodiscard]] const Impl& impl() const noexcept { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace fjell::gpu
