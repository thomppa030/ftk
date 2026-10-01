#pragma once

#include "ftk/base/handle.hpp"
#include "ftk/gpu/usage.hpp"

#include <concepts>
#include <cstdint>
#include <string_view>

namespace ftk::gpu {

class Device;
struct BufferTag;

/// A buffer, by handle. Made by `Device::create`, held by `Owned<Buffer>`.
using Buffer = Handle<BufferTag>;

/// Hands a buffer back to its device, which destroys it once the GPU is done
/// with it. `Owned<Buffer>` calls it.
void release(Device& device, Buffer buffer);

/// Part of a buffer, or the whole of it.
struct BufferRange {
    /// `size` reaching to the buffer's end.
    static constexpr uint64_t REST = UINT64_MAX;

    Buffer buffer{};
    uint64_t offset{0};
    uint64_t size{REST};

    constexpr BufferRange() = default;

    /// The whole buffer, from a `Buffer` or anything that holds one
    /// (`Owned<Buffer>`).
    template <typename T>
        requires std::convertible_to<const T&, Buffer>
    constexpr BufferRange(const T& whole) : buffer(static_cast<Buffer>(whole)) {}

    constexpr BufferRange(Buffer whole, uint64_t from, uint64_t bytes)
        : buffer(whole), offset(from), size(bytes) {}

    bool operator==(const BufferRange&) const = default;
};

/// Where a buffer's memory lives, which decides who may read and write it.
enum class Memory : uint8_t {
    /// Device memory. Filled by uploads, copies and shaders.
    gpu,
    /// Written by the CPU through `Device::mapped()` and read by the GPU.
    upload,
    /// Written by the GPU and read by the CPU through `Device::mapped()`.
    readback,
};

/// What `Device::create` makes a buffer from.
///
/// @code
/// auto lights = device.create(gpu::BufferDesc{
///     .size = sizeof(GpuLight) * MAX_LIGHTS,
///     .use = gpu::BufferUse::storage,
///     .memory = gpu::Memory::upload,
///     .name = "lights",
/// });
/// @endcode
struct BufferDesc {
    /// Bytes.
    uint64_t size{0};
    /// Bytes `Device::grow` may take the buffer to; 0 for a buffer that keeps
    /// its size. Only `Memory::gpu`. A device that can keeps the whole range
    /// for the buffer from the start and grows it in place; any other grows
    /// it by a copy into a larger one.
    uint64_t reserve{0};
    BufferUses use{};
    Memory memory{Memory::gpu};
    /// Shown by debuggers and validation messages; not kept.
    std::string_view name{};
};

} // namespace ftk::gpu
