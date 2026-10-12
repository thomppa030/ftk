#include "ftk/gpu/vulkan/command_list_impl.hpp"

#include "ftk/gpu/vulkan/access.hpp"
#include "ftk/gpu/vulkan/translate.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

// The render scope: `CommandList::render` and what the encoder records in it.

namespace ftk::gpu {

static_assert(sizeof(DrawArgs) == sizeof(VkDrawIndirectCommand) &&
              offsetof(DrawArgs, first_vertex) == offsetof(VkDrawIndirectCommand, firstVertex) &&
              offsetof(DrawArgs, first_instance) == offsetof(VkDrawIndirectCommand, firstInstance));
static_assert(sizeof(DrawIndexedArgs) == sizeof(VkDrawIndexedIndirectCommand) &&
              offsetof(DrawIndexedArgs, vertex_offset) == offsetof(VkDrawIndexedIndirectCommand, vertexOffset) &&
              offsetof(DrawIndexedArgs, first_instance) == offsetof(VkDrawIndexedIndirectCommand, firstInstance));
static_assert(sizeof(DrawMeshTasksArgs) == sizeof(VkDrawMeshTasksIndirectCommandEXT));

namespace {

// What rendering begins with, kept whole while `info` points into it.
struct Rendering {
    std::array<VkRenderingAttachmentInfo, MAX_COLOR_TARGETS> colors{};
    VkRenderingAttachmentInfo depth{};
    VkRenderingInfo info{};
    std::array<Format, MAX_COLOR_TARGETS> color_formats{};
    Format depth_format{Format::undefined};
    Samples samples{Samples::x1};
    VkRenderingFragmentShadingRateAttachmentInfoKHR shading_rate{};
};

// One attachment's texture, checked against the area drawn.
struct Attachment {
    VkImageView view{VK_NULL_HANDLE};
    Format format{Format::undefined};
    Samples samples{Samples::x1};
};

VkExtent2D mip_size(const TextureInfo& info, uint32_t level) {
    return {std::max(1U, info.width >> level), std::max(1U, info.height >> level)};
}

Result<Attachment> attachment(Device::Impl& device, const TextureView& view,
                              const std::string& what, VkRect2D area) {
    const Device::Impl::TextureRecord* record = device.textures.get(view.texture);
    if (record == nullptr) return make_error(what + " no longer exists");
    const ResolvedView resolved = resolve(view, record->info);
    const VkExtent2D size = mip_size(record->info, resolved.base_mip);
    if (uint64_t{size.width} < uint64_t{static_cast<uint32_t>(area.offset.x)} + area.extent.width ||
        uint64_t{size.height} < uint64_t{static_cast<uint32_t>(area.offset.y)} + area.extent.height) {
        return make_error(what + " is smaller than the area drawn");
    }
    const VkImageView native = device.image_view(view);
    if (native == VK_NULL_HANDLE) return make_error(what + " has no view");
    return Attachment{native, resolved.format, record->info.samples};
}

// Whether `color` is unbound: drawn to, with what is drawn dropped.
bool unbound(const ColorAttachment& color) { return !color.view.texture.valid(); }

// The area drawn: as given, reaching the first bound attachment's edge
// where no size is.
Result<VkRect2D> area_of(Device::Impl& device, const RenderTargets& targets) {
    const auto bound = std::ranges::find_if(targets.color, [](const ColorAttachment& color) { return !unbound(color); });
    if (bound == targets.color.end() && !targets.depth) return make_error("every target is unbound");
    const TextureView& first = bound != targets.color.end() ? bound->view : targets.depth->view;
    const Device::Impl::TextureRecord* record = device.textures.get(first.texture);
    if (record == nullptr) return make_error("the first target no longer exists");
    const VkExtent2D size = mip_size(record->info, resolve(first, record->info).base_mip);
    if (targets.x >= size.width || targets.y >= size.height) return make_error("the area starts outside the targets");
    return VkRect2D{{static_cast<int32_t>(targets.x), static_cast<int32_t>(targets.y)},
                    {targets.width != 0 ? targets.width : size.width - targets.x,
                     targets.height != 0 ? targets.height : size.height - targets.y}};
}

Result<> describe_colors(Device::Impl& device, const RenderTargets& targets, VkRect2D area,
                         Rendering& out) {
    bool samples_known = false;
    for (size_t i = 0; i < targets.color.size(); ++i) {
        const ColorAttachment& color = targets.color[i];
        if (unbound(color)) {
            // No view: writes to it are dropped, and any format a pipeline
            // names for it matches.
            VkRenderingAttachmentInfo& info = out.colors[i];
            info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            info.imageView = VK_NULL_HANDLE;
            out.color_formats[i] = Format::undefined;
            continue;
        }
        const std::string what = "colour target " + std::to_string(i);
        auto target = attachment(device, color.view, what, area);
        if (!target) return std::unexpected(target.error());
        const FormatKind format_kind = kind(target->format);
        if (format_kind != FormatKind::color && format_kind != FormatKind::color_uint) {
            return make_error(what + " is not a colour format");
        }
        if (!samples_known) {
            out.samples = target->samples;
            samples_known = true;
        }
        if (target->samples != out.samples) return make_error("the targets differ in samples");

        VkRenderingAttachmentInfo& info = out.colors[i];
        info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        info.imageView = target->view;
        info.imageLayout = vulkan::image_scope(Access::color_attachment, false).layout;
        info.loadOp = vulkan::to_vk(color.load);
        info.storeOp = vulkan::to_vk(color.store);
        info.clearValue = vulkan::to_vk(color.clear, target->format);
        if (color.resolve.texture.valid()) {
            auto into = attachment(device, color.resolve, what + "'s resolve target", area);
            if (!into) return std::unexpected(into.error());
            if (target->samples == Samples::x1 || into->samples != Samples::x1) {
                return make_error(what + " resolves from many samples into one");
            }
            // Whole numbers are not averaged.
            info.resolveMode = format_kind == FormatKind::color_uint
                                   ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
                                   : VK_RESOLVE_MODE_AVERAGE_BIT;
            info.resolveImageView = into->view;
            info.resolveImageLayout = info.imageLayout;
        }
        out.color_formats[i] = target->format;
    }
    return {};
}

Result<> describe_depth(Device::Impl& device, const DepthAttachment& depth, VkRect2D area,
                        bool first, Rendering& out) {
    auto target = attachment(device, depth.view, "the depth target", area);
    if (!target) return std::unexpected(target.error());
    if (kind(target->format) != FormatKind::depth) {
        return make_error("the depth target is not a depth format without stencil");
    }
    if (depth.access != Access::depth_attachment && depth.access != Access::depth_attachment_read &&
        depth.access != Access::depth_read_sampled) {
        return make_error("the depth target is used as no depth attachment access");
    }
    if (first) out.samples = target->samples;
    if (target->samples != out.samples) return make_error("the targets differ in samples");

    VkRenderingAttachmentInfo& info = out.depth;
    info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    info.imageView = target->view;
    info.imageLayout = vulkan::image_scope(depth.access, true).layout;
    info.loadOp = vulkan::to_vk(depth.load);
    // A depth only tested is not written, so ending the scope stores nothing:
    // a store would be a write the frame graph does not see, racing whatever
    // samples the depth next.
    info.storeOp = depth.access == Access::depth_attachment ? vulkan::to_vk(depth.store)
                                                             : VK_ATTACHMENT_STORE_OP_NONE;
    info.clearValue = vulkan::to_vk(depth.clear, target->format);
    if (depth.resolve.texture.valid()) {
        auto into = attachment(device, depth.resolve, "the depth resolve target", area);
        if (!into) return std::unexpected(into.error());
        if (target->samples == Samples::x1 || into->samples != Samples::x1) {
            return make_error("the depth target resolves from many samples into one");
        }
        info.resolveMode = vulkan::to_vk(depth.resolve_mode);
        info.resolveImageView = into->view;
        info.resolveImageLayout = vulkan::image_scope(Access::depth_resolve, true).layout;
    }
    out.depth_format = target->format;
    return {};
}

// The rate image, which must cover the area drawn in tiles of the GPU's size.
Result<> describe_shading_rate(Device::Impl& device, const TextureView& view, VkRect2D area, Rendering& out) {
    const ShadingRates& rates = device.caps.shading_rates;
    if (!rates.image) return make_error("the shading-rate image cannot be used: this GPU has none");
    const Device::Impl::TextureRecord* record = device.textures.get(view.texture);
    if (record == nullptr) return make_error("the shading-rate image no longer exists");
    const ResolvedView resolved = resolve(view, record->info);
    if (resolved.format != Format::r8_uint) return make_error("the shading-rate image is not r8_uint");
    if (record->info.samples != Samples::x1) return make_error("the shading-rate image has more than one sample");
    const VkExtent2D size = mip_size(record->info, resolved.base_mip);
    const uint64_t right = uint64_t{static_cast<uint32_t>(area.offset.x)} + area.extent.width;
    const uint64_t bottom = uint64_t{static_cast<uint32_t>(area.offset.y)} + area.extent.height;
    if (uint64_t{size.width} * rates.tile_width < right || uint64_t{size.height} * rates.tile_height < bottom) {
        return make_error("the shading-rate image is too small for the area drawn in " +
                          std::to_string(rates.tile_width) + " x " + std::to_string(rates.tile_height) + " tiles");
    }
    const VkImageView native = device.image_view(view);
    if (native == VK_NULL_HANDLE) return make_error("the shading-rate image has no view");
    out.shading_rate.sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR;
    out.shading_rate.imageView = native;
    out.shading_rate.imageLayout = vulkan::image_scope(Access::shading_rate_read, false).layout;
    out.shading_rate.shadingRateAttachmentTexelSize = {rates.tile_width, rates.tile_height};
    out.info.pNext = &out.shading_rate;
    return {};
}

Result<> describe(Device::Impl& device, const RenderTargets& targets, Rendering& out) {
    if (targets.color.empty() && !targets.depth) return make_error("there is nothing to draw to");
    if (targets.color.size() > MAX_COLOR_TARGETS) {
        return make_error("more than " + std::to_string(MAX_COLOR_TARGETS) + " colour targets");
    }
    auto area = area_of(device, targets);
    if (!area) return std::unexpected(area.error());
    if (auto colors = describe_colors(device, targets, *area, out); !colors) return colors;
    if (targets.depth) {
        const bool first = std::ranges::all_of(targets.color, unbound);
        auto depth = describe_depth(device, *targets.depth, *area, first, out);
        if (!depth) return depth;
    }
    if (targets.shading_rate.texture.valid()) {
        if (auto rate = describe_shading_rate(device, targets.shading_rate, *area, out); !rate) return rate;
    }

    out.info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    out.info.renderArea = *area;
    out.info.layerCount = 1;
    out.info.colorAttachmentCount = static_cast<uint32_t>(targets.color.size());
    out.info.pColorAttachments = out.colors.data();
    out.info.pDepthAttachment = targets.depth ? &out.depth : nullptr;
    return {};
}

// Whether a graphics pipeline draws to what the scope's targets are. An
// unbound target takes whatever the pipeline names for it where the device
// allows that (Caps::unbound_targets), else only a pipeline naming nothing.
Result<> matches_targets(const Device::Impl::PipelineRecord& pipeline, const CommandList::Impl& list,
                         bool unbound_targets) {
    const auto& colors = pipeline.color_formats;
    if (colors.size() != list.target_color_count) return make_error("draws to another number of colour targets");
    for (size_t i = 0; i < colors.size(); ++i) {
        if (colors[i] == list.target_colors[i]) continue;
        if (list.target_colors[i] != Format::undefined) {
            return make_error("draws to other colour formats than the targets are");
        }
        if (!unbound_targets) {
            return make_error("names a format for an unbound colour target, which this GPU cannot leave unbound");
        }
    }
    if (pipeline.depth_format != list.target_depth) {
        return make_error("tests another depth format than the target is");
    }
    if (pipeline.samples != list.target_samples) {
        return make_error("draws with other samples than the targets have");
    }
    if (list.target_rate_image && !pipeline.variable_rate) {
        return make_error("is not built variable_rate, which a scope holding a shading-rate image needs");
    }
    return {};
}

// Sends what the scope's draws shade at, under a pipeline that takes its
// rate per draw. The first combiner keeps the draw's rate over a rate a
// primitive could give (never set); the second takes the rate image's
// (REPLACE) or keeps the draw's own (KEEP).
void send_shading(Device& device, CommandList::Impl& list) {
    Device::Impl& impl = device.impl();
    if (list.pipeline == nullptr || !list.pipeline->variable_rate || impl.set_shading_rate == nullptr) return;
    const DrawShading sent = shading_for(impl.caps.shading_rates, list.target_samples, list.shading);
    const VkExtent2D size{sent.rate.width, sent.rate.height};
    const std::array<VkFragmentShadingRateCombinerOpKHR, 2> ops{
        VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
        sent.from_image ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR
                        : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
    impl.set_shading_rate(list.cb, &size, ops.data());
}

} // namespace

RenderEncoder CommandList::render(const RenderTargets& targets) {
    Impl& self = *impl_;
    if (!vulkan::outside_render(*device_, self, "begins rendering")) {
        return RenderEncoder(*device_, *this, false);
    }
    Rendering rendering;
    if (auto described = describe(device_->impl(), targets, rendering); !described) {
        device_->impl().report_once("Render targets: " + described.error());
        return RenderEncoder(*device_, *this, false);
    }
    vkCmdBeginRendering(self.cb, &rendering.info);

    const VkRect2D area = rendering.info.renderArea;
    const VkViewport viewport{static_cast<float>(area.offset.x), static_cast<float>(area.offset.y),
                              static_cast<float>(area.extent.width), static_cast<float>(area.extent.height),
                              0.0f, 1.0f};
    vkCmdSetViewport(self.cb, 0, 1, &viewport);
    vkCmdSetScissor(self.cb, 0, 1, &rendering.info.renderArea);

    self.rendering = true;
    self.pipeline = nullptr;
    self.bound_sets = 0;
    self.refused = false;
    self.target_colors = rendering.color_formats;
    self.target_color_count = rendering.info.colorAttachmentCount;
    self.target_depth = rendering.depth_format;
    self.target_samples = rendering.samples;
    self.target_rate_image = rendering.shading_rate.imageView != VK_NULL_HANDLE;
    self.vertex_buffer_set = false;
    self.index_buffer_set = false;
    self.shading = {.rate = {}, .from_image = true};
    return RenderEncoder(*device_, *this, true);
}

RenderEncoder::~RenderEncoder() {
    if (!open_) return;
    CommandList::Impl& self = list_->impl();
    vkCmdEndRendering(self.cb);
    self.rendering = false;
    self.pipeline = nullptr;
    self.bound_sets = 0;
    self.refused = false;
}

void RenderEncoder::set_pipeline(GraphicsPipeline pipeline) {
    if (!open_) return;
    CommandList::Impl& self = list_->impl();
    const Device::Impl::PipelineRecord* record = device_->impl().graphics_pipelines.get(pipeline);
    if (record != nullptr) {
        if (auto matches = matches_targets(*record, self, device_->impl().caps.unbound_targets); !matches) {
            self.pipeline = record;
            self.bound_sets = 0;
            vulkan::refuse(*device_, self, matches.error());
            return;
        }
    }
    vulkan::use_pipeline(*device_, self, record);
    send_shading(*device_, self);
}

void RenderEncoder::bind(BindGroup group) {
    if (open_) vulkan::bind_group(*device_, list_->impl(), group);
}

void RenderEncoder::bind(std::span<const BindEntry> entries) {
    if (open_) vulkan::bind_entries(*device_, list_->impl(), entries);
}

void RenderEncoder::push_bytes(std::span<const std::byte> bytes) {
    if (open_) vulkan::push(*device_, list_->impl(), bytes);
}

BufferRange RenderEncoder::transient_bytes(std::span<const std::byte> bytes) {
    return list_->transient(bytes);
}

void RenderEncoder::set_viewport(const Viewport& viewport) {
    if (!open_) return;
    const VkViewport native{viewport.x,     viewport.y,         viewport.width,
                            viewport.height, viewport.min_depth, viewport.max_depth};
    vkCmdSetViewport(list_->impl().cb, 0, 1, &native);
}

void RenderEncoder::set_scissor(const Rect& scissor) {
    if (!open_) return;
    const VkRect2D native{{scissor.x, scissor.y}, {scissor.width, scissor.height}};
    vkCmdSetScissor(list_->impl().cb, 0, 1, &native);
}

void RenderEncoder::set_shading_rate(ShadingRate rate) {
    if (!open_) return;
    CommandList::Impl& self = list_->impl();
    self.shading = {.rate = rate, .from_image = false};
    send_shading(*device_, self);
}

void RenderEncoder::set_shading_rate_from_image() {
    if (!open_) return;
    CommandList::Impl& self = list_->impl();
    self.shading = {.rate = {}, .from_image = true};
    send_shading(*device_, self);
}

void RenderEncoder::set_vertex_buffer(BufferRange vertices) {
    if (!open_) return;
    CommandList::Impl& self = list_->impl();
    const auto* buffer = device_->impl().buffers.get(vertices.buffer);
    if (buffer == nullptr) {
        vulkan::refuse(*device_, self, "reads vertices from a buffer that no longer exists");
        return;
    }
    vkCmdBindVertexBuffers(self.cb, 0, 1, &buffer->buffer, &vertices.offset);
    self.vertex_buffer_set = true;
}

void RenderEncoder::set_index_buffer(BufferRange indices, IndexType type) {
    if (!open_) return;
    CommandList::Impl& self = list_->impl();
    const auto* buffer = device_->impl().buffers.get(indices.buffer);
    if (buffer == nullptr) {
        vulkan::refuse(*device_, self, "reads indices from a buffer that no longer exists");
        return;
    }
    vkCmdBindIndexBuffer(self.cb, buffer->buffer, indices.offset, vulkan::to_vk(type));
    self.index_buffer_set = true;
}

namespace {

// Whether the pipeline may draw vertices: a vertex pipeline, its vertex
// buffer set if it reads one, and for an indexed draw its index buffer.
bool vertices_ready(Device& device, CommandList::Impl& list, bool indexed) {
    if (!vulkan::ready(device, list, "draws")) return false;
    const auto& pipeline = *list.pipeline;
    if (pipeline.mesh) {
        vulkan::refuse(device, list, "draws vertices with a mesh pipeline");
        return false;
    }
    if (pipeline.reads_vertices && !list.vertex_buffer_set) {
        vulkan::refuse(device, list, "draws with no vertex buffer set");
        return false;
    }
    if (indexed && !list.index_buffer_set) {
        vulkan::refuse(device, list, "draws indexed with no index buffer set");
        return false;
    }
    return true;
}

// Whether the pipeline may draw mesh tasks, `max_groups` of them at most.
bool mesh_ready(Device& device, CommandList::Impl& list, uint32_t max_groups) {
    if (!vulkan::ready(device, list, "draws")) return false;
    if (!list.pipeline->mesh) {
        vulkan::refuse(device, list, "draws mesh tasks with a vertex pipeline");
        return false;
    }
    if (max_groups == 0) {
        vulkan::refuse(device, list,
                       "draws mesh tasks without the most task groups a draw asks for");
        return false;
    }
    if (!list.pipeline->task && max_groups > device.caps().mesh_max_workgroups) {
        vulkan::refuse(device, list,
                       "draws up to " + std::to_string(max_groups) + " mesh workgroups at once; this GPU launches " +
                           std::to_string(device.caps().mesh_max_workgroups));
        return false;
    }
    return true;
}

} // namespace

void RenderEncoder::draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
                         uint32_t first_instance) {
    if (!open_ || !vertices_ready(*device_, list_->impl(), false)) return;
    vkCmdDraw(list_->impl().cb, vertex_count, instance_count, first_vertex, first_instance);
}

void RenderEncoder::draw_indexed(uint32_t index_count, uint32_t instance_count,
                                 uint32_t first_index, int32_t vertex_offset,
                                 uint32_t first_instance) {
    if (!open_ || !vertices_ready(*device_, list_->impl(), true)) return;
    vkCmdDrawIndexed(list_->impl().cb, index_count, instance_count, first_index, vertex_offset,
                     first_instance);
}

void RenderEncoder::draw_indirect(BufferRange args, uint32_t count, uint32_t stride) {
    if (!open_ || !vertices_ready(*device_, list_->impl(), false)) return;
    const auto* buffer = device_->impl().buffers.get(args.buffer);
    if (buffer == nullptr) {
        vulkan::refuse(*device_, list_->impl(),
                       "draws from arguments in a buffer that no longer exists");
        return;
    }
    vkCmdDrawIndirect(list_->impl().cb, buffer->buffer, args.offset, count, stride);
}

void RenderEncoder::draw_indexed_indirect(BufferRange args, uint32_t count, uint32_t stride) {
    if (!open_ || !vertices_ready(*device_, list_->impl(), true)) return;
    const auto* buffer = device_->impl().buffers.get(args.buffer);
    if (buffer == nullptr) {
        vulkan::refuse(*device_, list_->impl(),
                       "draws from arguments in a buffer that no longer exists");
        return;
    }
    vkCmdDrawIndexedIndirect(list_->impl().cb, buffer->buffer, args.offset, count, stride);
}

void RenderEncoder::draw_mesh_tasks(uint32_t x, uint32_t y, uint32_t z) {
    if (!open_ || !mesh_ready(*device_, list_->impl(), 1)) return;
    device_->impl().draw_mesh_tasks(list_->impl().cb, x, y, z);
}

void RenderEncoder::draw_mesh_tasks_indirect(BufferRange args, uint32_t draws, uint32_t stride,
                                             uint32_t max_groups) {
    if (!open_ || !mesh_ready(*device_, list_->impl(), max_groups)) return;
    const auto* buffer = device_->impl().buffers.get(args.buffer);
    if (buffer == nullptr) {
        vulkan::refuse(*device_, list_->impl(),
                       "draws from arguments in a buffer that no longer exists");
        return;
    }
    device_->impl().draw_mesh_tasks_indirect(list_->impl().cb, buffer->buffer, args.offset, draws,
                                             stride);
}

void RenderEncoder::draw_mesh_tasks_indirect_count(BufferRange args, BufferRange count,
                                                   uint32_t max_draws, uint32_t stride,
                                                   uint32_t max_groups) {
    if (!open_ || !mesh_ready(*device_, list_->impl(), max_groups)) return;
    const auto* arg_buffer = device_->impl().buffers.get(args.buffer);
    const auto* count_buffer = device_->impl().buffers.get(count.buffer);
    if (arg_buffer == nullptr || count_buffer == nullptr) {
        vulkan::refuse(*device_, list_->impl(), "draws from a buffer that no longer exists");
        return;
    }
    device_->impl().draw_mesh_tasks_indirect_count(list_->impl().cb, arg_buffer->buffer,
                                                   args.offset, count_buffer->buffer, count.offset,
                                                   max_draws, stride);
}

} // namespace ftk::gpu
