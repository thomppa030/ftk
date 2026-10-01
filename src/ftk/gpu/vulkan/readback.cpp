#include "ftk/gpu/readback.hpp"

#include "ftk/gpu/command_list.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/vulkan/device_impl.hpp"
#include "ftk/gpu/vulkan/native.hpp"
#include "ftk/gpu/vulkan/upload_lanes.hpp"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

// Readbacks on the Vulkan device: a compute pass samples the texture into a
// buffer the CPU maps, and a barrier hands the buffer to the host. It records
// on the upload context's image lane, whose timeline says when it is done, or
// into a frame's list, done with that frame.

namespace ftk::gpu {

namespace {

// The conversion shader, compiled into the library.
constexpr uint32_t READ_BACK_SPIRV[] = {
#include "read_back.comp.spv.inc"
};

struct ReadBackPush {
    std::array<float, 2> origin;
    std::array<float, 2> extent;
    std::array<uint32_t, 2> size;
};

// The shader's workgroup, square.
constexpr uint32_t GROUP = 8;

} // namespace

struct Readback::Impl {
    Device* device{nullptr};
    Owned<Buffer> pixels;
    Owned<BindGroup> group;
    /// The image lane batch the read went with: 0 until it is sent. None for
    /// a read in a frame's list.
    std::shared_ptr<const uint64_t> batch;
    /// The frame whose list holds the read; 0 for one on the image lane.
    uint64_t frame{0};
    uint32_t width{0};
    uint32_t height{0};
    /// Finished, with what the GPU wrote visible to the CPU.
    bool done{false};

    /// Makes what the GPU wrote visible to the CPU, once it has finished.
    void finish() {
        Device::Impl& self = device->impl();
        if (const auto* record = self.buffers.get(pixels)) {
            vmaInvalidateAllocation(self.allocator, record->allocation, 0, VK_WHOLE_SIZE);
        }
        done = true;
    }
};

Readback::Readback(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Readback::Readback(Readback&&) noexcept = default;

Readback& Readback::operator=(Readback&& other) noexcept {
    if (this != &other) {
        if (impl_ && impl_->frame == 0) wait();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Readback::~Readback() {
    // What it holds is released as it goes. A frame's read is kept by the
    // release until its frame is done; the image lane's is not, and the GPU
    // may still be writing it.
    if (impl_ && impl_->frame == 0) wait();
}

bool Readback::ready() const {
    if (!impl_) return false;
    if (impl_->done) return true;
    if (impl_->frame != 0) {
        if (impl_->device->finished_frame() < impl_->frame) return false;
    } else {
        vulkan::UploadLanes& lanes = impl_->device->impl().foundation.lanes();
        if (!lanes.image_done(*impl_->batch)) return false;
    }
    impl_->finish();
    return true;
}

void Readback::wait() {
    if (!impl_ || impl_->done) return;
    Device::Impl& self = impl_->device->impl();
    if (impl_->frame != 0) {
        if (self.ended < impl_->frame) {
            self.report_once("A readback waits for its frame before the frame is sent; it would never finish");
            return;
        }
        VkSemaphoreWaitInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        info.semaphoreCount = 1;
        info.pSemaphores = &self.frame_timeline;
        info.pValues = &impl_->frame;
        if (vkWaitSemaphores(self.device, &info, std::numeric_limits<uint64_t>::max()) != VK_SUCCESS) {
            self.report_once("Waiting for a readback's frame failed");
            return;
        }
    } else {
        vulkan::UploadLanes& lanes = self.foundation.lanes();
        if (*impl_->batch == 0) lanes.flush();
        lanes.wait_image(*impl_->batch);
    }
    impl_->finish();
}

std::span<const uint8_t> Readback::pixels() const {
    if (!ready()) return {};
    const std::span<std::byte> bytes = impl_->device->mapped(impl_->pixels);
    return {reinterpret_cast<const uint8_t*>(bytes.data()),
            static_cast<size_t>(impl_->width) * impl_->height * 4};
}

uint32_t Readback::width() const {
    return impl_ ? impl_->width : 0;
}

uint32_t Readback::height() const {
    return impl_ ? impl_->height : 0;
}

namespace {

// Records into `cmd` the read of `view` that `desc` asks for, and makes what
// it writes into.
Result<std::unique_ptr<Readback::Impl>> record_read(Device& device, CommandList& cmd, const TextureView& view,
                                                    const ReadbackDesc& desc) {
    Device::Impl& self = device.impl();
    const auto* record = self.textures.get(view.texture);
    if (record == nullptr) return make_error("Readback: the texture no longer exists");
    const ResolvedView resolved = resolve(view, record->info);
    if (resolved.mip_count != 1 || resolved.layer_count != 1) {
        return make_error("Readback: one mip of one layer is read at a time");
    }
    if (kind(resolved.format) != FormatKind::color || record->info.kind == TextureKind::tex3d) {
        return make_error("Readback: only a 2D colour texture read as floats is read back");
    }
    if (!record->info.use.has(TextureUse::sampled)) {
        return make_error("Readback: the texture is not made to be sampled");
    }
    if (desc.use.empty()) return make_error("Readback: what the texture is used as is not given");

    const uint32_t mip_width = std::max(1U, record->info.width >> resolved.base_mip);
    const uint32_t mip_height = std::max(1U, record->info.height >> resolved.base_mip);
    const TextureRegion box =
        desc.region.width == 0 ? TextureRegion{.width = mip_width, .height = mip_height} : desc.region;
    if (box.height == 0 || box.x + box.width > mip_width || box.y + box.height > mip_height) {
        return make_error("Readback: the region does not fit in the mip");
    }
    const uint32_t width = desc.width != 0 ? desc.width : box.width;
    const uint32_t height = desc.height != 0 ? desc.height : box.height;

    if (!self.read_back_pipeline.valid()) {
        auto built = self.build(ComputePipelineDesc{
            .shader = ShaderCode(std::span<const uint32_t>(READ_BACK_SPIRV)),
            .name = "read_back",
        });
        if (!built) return std::unexpected(built.error());
        self.read_back_pipeline = self.compute_pipelines.emplace(std::move(*built));
    }

    auto impl = std::make_unique<Readback::Impl>();
    impl->device = &device;
    impl->width = width;
    impl->height = height;
    auto pixels = device.create(BufferDesc{
        .size = uint64_t{width} * height * 4,
        .use = BufferUse::storage,
        .memory = Memory::readback,
        .name = "readback",
    });
    if (!pixels) return std::unexpected(pixels.error());
    impl->pixels = std::move(*pixels);
    auto group = device.create(BindGroupDesc{
        .pipeline = self.read_back_pipeline,
        .entries = {{"source", sampled(view, device.sampler({.filter = Filter::linear,
                                                              .address = Address::clamp}))},
                    {"pixels", storage(impl->pixels)}},
        .name = "readback",
    });
    if (!group) return std::unexpected(group.error());
    impl->group = std::move(*group);

    cmd.barrier(view, desc.use, Access::sampled_compute);
    cmd.set_pipeline(self.read_back_pipeline);
    cmd.bind(impl->group);
    cmd.push(ReadBackPush{
        .origin = {static_cast<float>(box.x) / static_cast<float>(mip_width),
                   static_cast<float>(box.y) / static_cast<float>(mip_height)},
        .extent = {static_cast<float>(box.width) / static_cast<float>(mip_width),
                   static_cast<float>(box.height) / static_cast<float>(mip_height)},
        .size = {width, height},
    });
    cmd.dispatch((width + GROUP - 1) / GROUP, (height + GROUP - 1) / GROUP, 1);
    cmd.barrier(view, Access::sampled_compute, desc.use);
    cmd.barrier(BufferRange(impl->pixels), Access::storage_buffer_write_compute, Access::host_read);
    return impl;
}

} // namespace

Result<Readback> Device::read_back(const TextureView& view, const ReadbackDesc& desc) {
    vulkan::UploadLanes& lanes = impl_->foundation.lanes();
    vulkan::CommandBufferList recorder(*this, lanes.image_cb());
    auto impl = record_read(*this, recorder.list(), view, desc);
    if (!impl) return std::unexpected(impl.error());
    (*impl)->batch = lanes.image_batch();
    return Readback(std::move(*impl));
}

Result<Readback> CommandList::read_back(const TextureView& view, const ReadbackDesc& desc) {
    const uint64_t frame = device_->impl().recording;
    if (frame == 0 || device_->impl().ended >= frame) {
        return make_error("Readback: a list reads back in the frame being recorded, and none is");
    }
    auto impl = record_read(*device_, *this, view, desc);
    if (!impl) return std::unexpected(impl.error());
    (*impl)->frame = frame;
    return Readback(std::move(*impl));
}

} // namespace ftk::gpu
