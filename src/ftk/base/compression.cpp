#include "ftk/base/compression.hpp"

#include <zstd.h>

#include <algorithm>
#include <string>

namespace ftk {

std::vector<std::byte> compress(std::span<const std::byte> bytes, int level) {
    level = std::clamp(level, FASTEST_COMPRESSION, SMALLEST_COMPRESSION);
    std::vector<std::byte> frame(ZSTD_compressBound(bytes.size()));
    const size_t written = ZSTD_compress(frame.data(), frame.size(), bytes.data(), bytes.size(), level);
    // Compressing into a buffer of ZSTD_compressBound() bytes cannot fail.
    frame.resize(ZSTD_isError(written) != 0 ? 0 : written);
    return frame;
}

std::optional<uint64_t> decompressed_size(std::span<const std::byte> frame) {
    const unsigned long long size = ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR) return std::nullopt;
    return static_cast<uint64_t>(size);
}

Result<> decompress(std::span<const std::byte> frame, std::span<std::byte> out) {
    const std::optional<uint64_t> size = decompressed_size(frame);
    if (!size) return make_error("not a zstd frame that says its size");
    if (*size != out.size()) {
        return make_error("the frame decompresses to " + std::to_string(*size) + " bytes, not " +
                          std::to_string(out.size()));
    }
    const size_t read = ZSTD_decompress(out.data(), out.size(), frame.data(), frame.size());
    if (ZSTD_isError(read) != 0) return make_error(std::string("the frame is damaged: ") + ZSTD_getErrorName(read));
    if (read != out.size()) return make_error("the frame decompressed short of its size");
    return {};
}

} // namespace ftk
