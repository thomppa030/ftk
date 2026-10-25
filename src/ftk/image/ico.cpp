#include "ftk/image/ico.hpp"
#include "ftk/image/png.hpp"
#include "ftk/image/svg.hpp"

#include <cstdint>

namespace ftk::image {

namespace {

// An ICO file's header, then a 16-byte entry per picture, then the pictures.
constexpr std::size_t HEADER = 6;
constexpr std::size_t ENTRY = 16;

void put16(std::vector<std::byte>& out, uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFu));
    out.push_back(static_cast<std::byte>(value >> 8));
}

void put32(std::vector<std::byte>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
}

} // namespace

Result<std::vector<Rgba8>> rasterize_icon(std::string_view svg) {
    std::vector<Rgba8> sizes;
    sizes.reserve(ICON_SIZES.size());
    for (const uint32_t size : ICON_SIZES) {
        auto drawn = rasterize_svg(svg, size, size);
        if (!drawn) return std::unexpected(std::move(drawn).error());
        sizes.push_back(std::move(*drawn));
    }
    return sizes;
}

std::vector<std::byte> encode_ico(std::span<const Rgba8> sizes) {
    if (sizes.empty() || sizes.size() > 0xFFFFu) return {};
    std::vector<std::vector<std::byte>> pngs;
    pngs.reserve(sizes.size());
    for (const Rgba8& picture : sizes) {
        if (picture.width != picture.height || picture.width == 0 || picture.width > 256) return {};
        pngs.push_back(encode_png(picture.texels, picture.width, picture.height, 4));
        if (pngs.back().empty()) return {};
    }

    std::vector<std::byte> out;
    put16(out, 0);                                   // reserved
    put16(out, 1);                                   // an icon, not a cursor
    put16(out, static_cast<uint16_t>(sizes.size()));
    std::size_t offset = HEADER + ENTRY * sizes.size();
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        // A side of 256 is written as 0: the field holds a byte.
        const auto side = static_cast<std::byte>(sizes[i].width == 256 ? 0 : sizes[i].width);
        out.push_back(side);
        out.push_back(side);
        out.push_back(std::byte{0});                 // no palette
        out.push_back(std::byte{0});                 // reserved
        put16(out, 1);                               // colour planes
        put16(out, 32);                              // bits a texel
        put32(out, static_cast<uint32_t>(pngs[i].size()));
        put32(out, static_cast<uint32_t>(offset));
        offset += pngs[i].size();
    }
    for (const std::vector<std::byte>& png : pngs) out.insert(out.end(), png.begin(), png.end());
    return out;
}

} // namespace ftk::image
