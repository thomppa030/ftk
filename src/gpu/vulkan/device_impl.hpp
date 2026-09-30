#pragma once

#include "core/handle_pool.hpp"
#include "gpu/device.hpp"
#include "gpu/release_queue.hpp"
#include "gpu/transient_memory.hpp"
#include "gpu/upload.hpp"
#include "gpu/vulkan/frame_descriptor_cache.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#ifdef FJELL_ENABLE_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <filesystem>
#include <vector>

namespace fjell {
class GpuCore;
class UploadContext;
}

namespace fjell::gpu {

struct FrameSlot;

/// The Vulkan backend's upload: the lanes `GpuCore` runs, which frames wait
/// for.
struct Upload::Impl {
    UploadContext& lanes;
};

/// The Vulkan backend's device: pools of native objects behind the handles,
/// over what `GpuCore` already brought up (the VkDevice, the allocator and
/// the upload lanes).
struct Device::Impl {
    struct BufferRecord {
        VkBuffer buffer{VK_NULL_HANDLE};
        /// Null for an adopted buffer, which the device does not destroy,
        /// and for a sparse one, whose memory is `pages`.
        VmaAllocation allocation{VK_NULL_HANDLE};
        /// Usable bytes: for a sparse buffer, those with memory bound.
        uint64_t size{0};
        std::byte* mapped{nullptr};
        /// What `grow` may take it to; 0 for a buffer that keeps its size.
        /// A sparse buffer is made this large, rounded to whole pages.
        uint64_t reserve{0};
        /// Made with its reserve and given memory as it grows.
        bool sparse{false};
        /// What a sparse buffer's memory must be: page size and memory
        /// types. The memory itself, bound in order from the start.
        VkMemoryRequirements page_requirements{};
        std::vector<VmaAllocation> pages;
        /// What it was made with, for the larger buffer a copy grows into.
        VkBufferUsageFlags usage{0};
        std::string name;
        /// Counts the copies that replaced it.
        uint32_t generation{0};
    };

    struct View {
        ResolvedView key;
        VkImageView view{VK_NULL_HANDLE};
        /// False for the whole view an adopted image came with.
        bool owned{true};
    };

    /// What a pipeline layout is made of.
    struct LayoutInfo {
        /// Shared with every pipeline whose shaders declare the same; the
        /// device keeps it.
        VkPipelineLayout layout{VK_NULL_HANDLE};
        /// One per set up to the highest declared, the device's or shared.
        std::vector<VkDescriptorSetLayout> set_layouts;
        /// The set each shared layout the pipeline names took.
        std::vector<std::pair<SharedLayout, uint32_t>> shared_sets;
    };

    struct PipelineRecord {
        VkPipeline pipeline{VK_NULL_HANDLE};
        LayoutInfo layout;
        VkPipelineBindPoint bind_point{VK_PIPELINE_BIND_POINT_COMPUTE};
        ShaderLayout shader_layout;
        /// The sets its shaders declare, one bit each: what must be bound
        /// before it runs.
        uint32_t declared_sets{0};
        /// What names it in errors: "Compute pipeline 'name'".
        std::string name;
        /// A graphics pipeline's: what it draws to, whether it is a mesh
        /// pipeline, and whether it reads a vertex buffer.
        std::vector<Format> color_formats;
        Format depth_format{Format::undefined};
        Samples samples{Samples::x1};
        bool mesh{false};
        bool reads_vertices{false};
    };

    struct SharedRecord {
        SharedLayoutDesc desc;
        VkDescriptorSetLayout layout{VK_NULL_HANDLE};
        /// Where each binding's elements start in a group's contents, how many
        /// descriptors of each type one set holds, and the pools its groups'
        /// sets come from, each holding a few whole sets.
        std::vector<uint32_t> first;
        std::vector<VkDescriptorPoolSize> set_sizes;
        std::vector<VkDescriptorPool> pools;
    };

    /// What a shared group keeps besides its set.
    struct SharedGroupState {
        /// Every element's resource, empty where none was given, in the
        /// layout's binding order: what a new version is written from.
        std::vector<std::optional<BindResource>> contents;
        /// The serial of the last frame that recorded a bind of the current
        /// set; `NEVER_BOUND` until one does. An update writes the set in
        /// place once that frame is finished, and a new version before.
        std::atomic<uint64_t> bound{NEVER_BOUND};
        std::string name;

        static constexpr uint64_t NEVER_BOUND = UINT64_MAX;
    };

    struct GroupRecord {
        VkDescriptorSet set{VK_NULL_HANDLE};
        /// The pool it came from.
        VkDescriptorPool pool{VK_NULL_HANDLE};
        VkDescriptorSetLayout layout{VK_NULL_HANDLE};
        /// The one set's bindings, what `update` checks entries against.
        ShaderLayout bindings;
        /// The set of its pipeline it fills; for a shared group, the set a
        /// pipeline naming its layout takes instead.
        uint32_t set_index{0};
        /// For a shared group, the shared layout it is.
        SharedLayout shared{};
        /// A shared group's; null for a pipeline's own.
        std::unique_ptr<SharedGroupState> state;
    };

    struct TextureRecord {
        VkImage image{VK_NULL_HANDLE};
        /// Null for an adopted image, which the device does not destroy.
        VmaAllocation allocation{VK_NULL_HANDLE};
        TextureInfo info;
        /// The aspect a barrier on the texture names: depth alone for any
        /// depth format, as a view shows it, else colour. Kept from the
        /// native format, which an adopted image may have outside `Format`.
        VkImageAspectFlags aspect{VK_IMAGE_ASPECT_COLOR_BIT};
        /// Made on first use, destroyed with the texture. Guarded by
        /// `views_mutex`: any recording thread may ask for a view.
        std::vector<View> views;
        /// Run when the texture is destroyed, once the frames using it are
        /// done, before its views go: what else holds one (an ImGui image)
        /// lets it go. Guarded by `views_mutex`.
        std::vector<std::move_only_function<void()>> on_release;
    };

    /// Memory acceleration structures made together share, freed with the
    /// last of them. A dedicated allocation, never a range of a shared
    /// block: a freed neighbour's range handed on and written while a frame
    /// still traced structures beside it once faulted traversal and lost the
    /// device. Structures made together share one rather than one each,
    /// which would count against the GPU's limit on allocations.
    struct AccelerationMemory {
        VmaAllocator allocator{VK_NULL_HANDLE};
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};

        AccelerationMemory() = default;
        AccelerationMemory(const AccelerationMemory&) = delete;
        AccelerationMemory& operator=(const AccelerationMemory&) = delete;
        ~AccelerationMemory();
    };

    struct AccelerationRecord {
        VkAccelerationStructureKHR structure{VK_NULL_HANDLE};
        std::shared_ptr<AccelerationMemory> memory;
        /// What instance records name it by.
        VkDeviceAddress address{0};
        bool top{false};
        AccelerationUse use{AccelerationUse::traced};
        /// What it was made for, which a build may not exceed: a bottom
        /// level's triangles and vertices, a top level's instances.
        uint32_t primitives{0};
        uint32_t vertices{0};
        /// Scratch memory a build of it takes.
        uint64_t build_scratch{0};
        std::string name;
    };

    explicit Impl(GpuCore& core);
    ~Impl();

    /// Writes the pipeline cache to its file and destroys it; nothing
    /// without one.
    void save_pipeline_cache();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    /// A buffer as described, which its maker releases or destroys: what
    /// `Device::create` wraps, and what transient chunks are made with.
    [[nodiscard]] Result<Buffer> make_buffer(const BufferDesc& desc);

    /// Where a buffer made with `BufferUse::device_address` or
    /// `acceleration_input` starts, as shaders and builds address it.
    [[nodiscard]] VkDeviceAddress buffer_address(VkBuffer buffer) const;

    /// The native description of a build of `record`: its level, what it
    /// is made for, and preferences; geometry filled in by the caller.
    [[nodiscard]] VkAccelerationStructureBuildGeometryInfoKHR build_info(
        const AccelerationRecord& record, const VkAccelerationStructureGeometryKHR& geometry) const;

    /// Binds memory to a sparse buffer from its end up to `target` bytes,
    /// in whole pages and no further than its reserve, and waits for the
    /// bind.
    [[nodiscard]] Result<> bind_pages(BufferRecord& record, uint64_t target);

    /// The native view for `view`, made the first time it is asked for.
    /// Null when the handle finds no texture or the view cannot be made.
    [[nodiscard]] VkImageView image_view(const TextureView& view);

    /// A compiled shader's words, read through the locator for a path.
    [[nodiscard]] Result<std::vector<uint32_t>> load(const ShaderCode& code) const;

    /// The pipeline layout for what `layout` binds and takes, with the shared
    /// layouts `shared` at the sets they fit, made the first time a pipeline
    /// declares it and shared after.
    [[nodiscard]] Result<LayoutInfo> pipeline_layout(const ShaderLayout& layout,
                                                     std::span<const SharedLayout> shared);

    /// A descriptor set of `layout` from the device's group pools, adding a
    /// pool when the last is full. Null when none can be made.
    [[nodiscard]] std::pair<VkDescriptorSet, VkDescriptorPool> allocate_set(VkDescriptorSetLayout layout);

    /// A descriptor set of a made shared layout from its own pools, adding a
    /// pool when every one is full. Null when none can be made.
    [[nodiscard]] std::pair<VkDescriptorSet, VkDescriptorPool> allocate_shared(SharedRecord& shared);

    /// Whether the resource still exists.
    [[nodiscard]] bool exists(const BindResource& resource);

    /// Whether every resource the placed entries name still exists: a stale
    /// handle would write a descriptor the GPU then reads as nothing.
    /// @return nothing, or which binding is given a resource that is gone.
    [[nodiscard]] Result<> check_resources(const PlacedSet& placed);

    /// One placed entry as the descriptor that is written for it.
    [[nodiscard]] FrameCacheBinding describe(const PlacedEntry& entry);

    /// Writes placed entries into `set`.
    void write_set(VkDescriptorSet set, const PlacedSet& placed);

    /// Why `set` of `pipeline` cannot take a group of its own: a shared
    /// layout takes it, bound through the engine's shared group. Empty when
    /// the set is the pipeline's own.
    [[nodiscard]] std::string shared_at(const PipelineRecord& pipeline, uint32_t set);

    /// Runs `fn` once the GPU has finished the frame recording now: how
    /// everything the device destroys is destroyed.
    void release_later(std::move_only_function<void()> fn) {
        releases.defer(recording, std::move(fn));
    }

    /// Logs `message` as an error the first time it is reported, so a
    /// mistake recorded every frame is read once.
    void report_once(const std::string& message);

    [[nodiscard]] Result<PipelineRecord> build(const ComputePipelineDesc& desc);
    [[nodiscard]] Result<PipelineRecord> build(const GraphicsPipelineDesc& desc);

    GpuCore& core;
    VkDevice device{VK_NULL_HANDLE};
    VmaAllocator allocator{VK_NULL_HANDLE};
    float max_anisotropy{1.0f};
    /// The queue sparse memory is bound on; null where the device cannot
    /// bind it, and growable buffers grow by copying. The fence each bind
    /// is waited on with, made at the first.
    VkQueue sparse_queue{VK_NULL_HANDLE};
    VkFence sparse_fence{VK_NULL_HANDLE};

    HandlePool<BufferRecord, BufferTag> buffers;
    HandlePool<TextureRecord, TextureTag> textures;
    HandlePool<VkSampler, SamplerTag> samplers;
    HandlePool<PipelineRecord, ComputePipelineTag> compute_pipelines;
    HandlePool<PipelineRecord, GraphicsPipelineTag> graphics_pipelines;
    HandlePool<SharedRecord, SharedLayoutTag> shared_layouts;
    HandlePool<GroupRecord, BindGroupTag> groups;
    HandlePool<AccelerationRecord, AccelerationStructureTag> accelerations;
    /// Pools persistent groups come from, each allowing sets to be freed.
    std::vector<VkDescriptorPool> group_pools;
    /// Sets that last one frame, from a pool per frame slot reset when the
    /// slot comes round again; deduplicated within a frame.
    FrameDescriptorCache frame_sets;
    /// The frame slot being recorded, set where each frame starts.
    uint32_t frame_slot{0};
    /// The timeline each frame's last submission signals at the frame's
    /// serial: the value it has reached is the newest frame the GPU has
    /// finished.
    VkSemaphore frame_timeline{VK_NULL_HANDLE};
    /// The serial of the frame recording now, which what is released is
    /// kept alive for.
    uint64_t recording{0};
    /// The serial of the newest frame sent to the GPU.
    uint64_t ended{0};
    /// The frames in flight, one per slot, made when first used, and the
    /// serial of the last frame begun.
    std::vector<std::unique_ptr<FrameSlot>> frames;
    uint64_t frame_serial{0};
    /// Per queue (graphics, compute): the queue, its family, and a timeline
    /// every submission on it signals one count higher, which is how a list
    /// on one queue waits for one on the other.
    std::array<VkQueue, 2> queues{};
    std::array<uint32_t, 2> families{};
    std::array<VkSemaphore, 2> queue_timelines{};
    std::array<uint64_t, 2> queue_values{};
    /// What was released and waits for the GPU to finish the frames that
    /// may still use it.
    ReleaseQueue releases;
    /// The device's upload, made when first asked for.
    Upload::Impl upload_state;
    std::unique_ptr<Upload> upload;
    /// What converts a texture read back, made at the first readback and
    /// destroyed with the device.
    ComputePipeline read_back_pipeline{};
    /// Memory that lasts one frame, reset with `frame_sets`. Its chunks are
    /// the device's own, destroyed with it.
    TransientMemory transient;
    /// Set and pipeline layouts by what they hold, so pipelines declaring the
    /// same share one. Destroyed with the device.
    std::unordered_map<std::string, VkDescriptorSetLayout> set_layouts;
    std::unordered_map<std::string, VkPipelineLayout> pipeline_layouts;
    /// Labels debuggers show around a zone, loaded where debug utils are;
    /// null elsewhere.
    PFN_vkCmdBeginDebugUtilsLabelEXT begin_label{nullptr};
    PFN_vkCmdEndDebugUtilsLabelEXT end_label{nullptr};
#ifdef FJELL_ENABLE_TRACY
    /// Where a zone was opened: its name and its place in the source.
    struct ZoneSite {
        std::string name;
        const char* file{nullptr};
        uint32_t line{0};
    };
    struct ZoneSiteKey {
        std::string_view name;
        const char* file{nullptr};
        uint32_t line{0};
    };
    struct ZoneSiteHash {
        using is_transparent = void;
        size_t operator()(const ZoneSiteKey& key) const noexcept;
        size_t operator()(const ZoneSite& site) const noexcept {
            return (*this)(ZoneSiteKey{site.name, site.file, site.line});
        }
    };
    struct ZoneSiteEqual {
        using is_transparent = void;
        template <typename A, typename B>
        bool operator()(const A& a, const B& b) const noexcept {
            return std::string_view(a.name) == std::string_view(b.name) && a.file == b.file &&
                   a.line == b.line;
        }
    };

    /// The profiler's GPU context on the graphics queue.
    tracy::VkCtx* profiler{nullptr};
    /// Every zone site seen, which the profiler keeps pointing at.
    std::unordered_map<ZoneSite, tracy::SourceLocationData, ZoneSiteHash, ZoneSiteEqual> zone_sites;
    std::mutex zone_sites_mutex;

    /// The profiler's record of a zone site, made the first time it opens.
    [[nodiscard]] const tracy::SourceLocationData* zone_source(std::string_view name,
                                                               const std::source_location& where);
#endif
    /// Acceleration structures, loaded where the GPU has ray queries; null
    /// elsewhere.
    PFN_vkCreateAccelerationStructureKHR create_acceleration{nullptr};
    PFN_vkDestroyAccelerationStructureKHR destroy_acceleration{nullptr};
    PFN_vkGetAccelerationStructureBuildSizesKHR acceleration_sizes{nullptr};
    PFN_vkCmdBuildAccelerationStructuresKHR build_acceleration{nullptr};
    PFN_vkGetAccelerationStructureDeviceAddressKHR acceleration_address{nullptr};
    /// Where a build's scratch starts: a multiple of this.
    uint64_t scratch_alignment{256};
    /// What builds work in: memory only the GPU sees, a slot's handed out
    /// anew when it comes round, the chunks the device's own. A build
    /// larger than a chunk takes memory of its own for its frame.
    TransientMemory scratch;
    /// Mesh draws, loaded where the GPU has mesh shaders; null elsewhere.
    PFN_vkCmdDrawMeshTasksEXT draw_mesh_tasks{nullptr};
    PFN_vkCmdDrawMeshTasksIndirectEXT draw_mesh_tasks_indirect{nullptr};
    PFN_vkCmdDrawMeshTasksIndirectCountEXT draw_mesh_tasks_indirect_count{nullptr};
    /// What pipelines are built through, loaded from and saved to
    /// `pipeline_cache_file`; null without one.
    VkPipelineCache pipeline_cache{VK_NULL_HANDLE};
    std::filesystem::path pipeline_cache_file;
    ShaderLocator locator;
    Caps caps;
    /// Every sampler made, by its description; few enough to search.
    std::vector<std::pair<SamplerDesc, Sampler>> sampler_by_desc;

    /// What `report_once` has logged.
    std::unordered_set<std::string> reported;

    /// Where `CommandList::transition` describes the barriers it records, a
    /// line each; null records nothing. Set by `vulkan::trace_transitions`.
    std::string* transition_trace{nullptr};

    std::mutex views_mutex;
    std::mutex samplers_mutex;
    std::mutex reported_mutex;
};

namespace vulkan {

/// Names a Vulkan object for validation messages and RenderDoc. Silent when
/// debug utils are not loaded.
void name_object(VkDevice device, VkObjectType type, uint64_t handle, std::string_view name);

/// The device over what `core` already brought up. `GpuCore` makes it last
/// and destroys it first, after the GPU is idle.
[[nodiscard]] std::unique_ptr<Device> create_device(GpuCore& core);

} // namespace vulkan

} // namespace fjell::gpu
