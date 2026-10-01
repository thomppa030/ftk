#pragma once

#include "ftk/gpu/access.hpp"
#include "ftk/gpu/buffer.hpp"
#include "ftk/gpu/clear.hpp"
#include "ftk/gpu/texture.hpp"

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <type_traits>

namespace ftk::gpu {

class Device;

/// A box of texels in one mip of a texture, from its corner nearest the
/// origin. A width of 0 is the whole mip.
struct TextureRegion {
    uint32_t x{0};
    uint32_t y{0};
    uint32_t z{0};
    uint32_t width{0};
    uint32_t height{1};
    uint32_t depth{1};
};

/// How `Upload::to_texture` writes a texture, and what it leaves it ready for.
struct TextureUploadDesc {
    /// What the texture has been used as, which the write waits for. Nothing
    /// for a texture that holds nothing yet: what it held is dropped, outside
    /// the region too.
    AccessSet before{};
    /// What it is used as afterwards.
    AccessSet after{Access::sampled_fragment};
    /// Fills every mip after the first from it, filtered. The write is then
    /// to the whole of mip 0.
    bool generate_mips{false};
};

/// Data from the CPU on its way into buffers and textures. What is written
/// lands before the work of the next frame to end: `Device::end_frame` sends
/// it first and the frame's lists wait for it. Nothing here waits for the
/// GPU; the bytes are copied away before each call returns.
///
/// Belongs to the thread that submits frames. `Device::upload()` is the
/// device's one.
///
/// @code
/// auto& upload = device.upload();
/// upload.to_buffer(vertices, 0, mesh.vertices);
/// upload.to_texture(albedo, {}, pixels, {.after = gpu::Access::sampled_fragment,
///                                       .generate_mips = true});
/// @endcode
class Upload {
public:
    /// The backend's state, which the backend defines.
    struct Impl;

    Upload(Device& device, Impl& impl) noexcept : device_(&device), impl_(&impl) {}

    Upload(const Upload&) = delete;
    Upload& operator=(const Upload&) = delete;

    /// Copies `bytes` into `dst` from `offset`. Writes to the same bytes of a
    /// buffer before the next frame are not ordered among themselves; a
    /// write that reaches past the buffer's end is reported and not made.
    void to_buffer(Buffer dst, uint64_t offset, std::span<const std::byte> bytes);

    /// `to_buffer` for the values of a contiguous range, one after another.
    template <std::ranges::contiguous_range R>
        requires std::is_trivially_copyable_v<std::ranges::range_value_t<R>>
    void to_buffer(Buffer dst, uint64_t offset, const R& values) {
        const std::span all(std::ranges::data(values), std::ranges::size(values));
        to_buffer(dst, offset, std::as_bytes(all));
    }

    /// Copies `bytes`, texels packed row after row, then slice after slice
    /// and layer after layer, into `region` of `dst`: a view of one mip and
    /// the layers it writes. A write that does not fit is reported and not
    /// made.
    void to_texture(const TextureView& dst, const TextureRegion& region,
                    std::span<const std::byte> bytes, const TextureUploadDesc& desc = {});

    /// Sets every texel of `view` to `value`, dropping what it held, and
    /// leaves it ready to be used as `after`.
    void clear(const TextureView& view, const Clear& value, AccessSet after);

    [[nodiscard]] Impl& impl() const noexcept { return *impl_; }

private:
    Device* device_;
    Impl* impl_;
};

} // namespace ftk::gpu
