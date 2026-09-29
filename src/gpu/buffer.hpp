#pragma once

#include "core/handle.hpp"
#include "gpu/usage.hpp"

#include <cstdint>
#include <string_view>

namespace fjell::gpu {

class Device;
struct BufferTag;

/// A buffer, by handle. Made by `Device::create`, held by `Owned<Buffer>`.
using Buffer = Handle<BufferTag>;

/// Hands a buffer back to its device, which destroys it once the GPU is done
/// with it. `Owned<Buffer>` calls it.
void release(Device& device, Buffer buffer);

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

} // namespace fjell::gpu
