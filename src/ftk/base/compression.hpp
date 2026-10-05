#pragma once

#include "ftk/base/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ftk {

/// How hard compress() works: zstd's levels, from fastest to smallest.
inline constexpr int FASTEST_COMPRESSION = 1;
inline constexpr int DEFAULT_COMPRESSION = 3;
inline constexpr int SMALLEST_COMPRESSION = 19;

/// `bytes` as one zstd frame, which records the size it decompresses to.
/// `level` runs from FASTEST_COMPRESSION to SMALLEST_COMPRESSION. Any thread
/// may compress at once.
[[nodiscard]] std::vector<std::byte> compress(std::span<const std::byte> bytes, int level = DEFAULT_COMPRESSION);

/// The size a zstd frame says it decompresses to, or nothing for bytes that
/// are not a frame or do not say.
[[nodiscard]] std::optional<uint64_t> decompressed_size(std::span<const std::byte> frame);

/// Decompresses one zstd frame into `out`, which is exactly the size it
/// decompresses to. Any thread may decompress at once.
/// @return nothing, or why not: the bytes are not a whole frame, or it does
///         not decompress to `out`'s size
[[nodiscard]] Result<> decompress(std::span<const std::byte> frame, std::span<std::byte> out);

} // namespace ftk
