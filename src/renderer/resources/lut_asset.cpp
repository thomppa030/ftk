#include "renderer/resources/lut_asset.hpp"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>

// stb_image_write's zlib compressor. Public, but declared only inside its
// implementation, so the one PNG this file writes that stb cannot, 16 bits a
// channel, declares it here.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len,
                                             int quality);

namespace fjell {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t MIN_SIZE = 2;
constexpr uint32_t MAX_SIZE = 64;
// What a .cube is resampled onto: fine enough that a table made at 33 or 65
// keeps its shape, and a strip of 1024 is well inside any texture limit.
constexpr uint32_t CUBE_STRIP_SIZE = 32;

uint16_t to_unorm16(float v) {
    return static_cast<uint16_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f));
}

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

LutStrip neutral_lut(uint32_t size) {
    size = std::clamp(size, MIN_SIZE, MAX_SIZE);
    LutStrip strip;
    strip.size = size;
    strip.rgba.resize(static_cast<size_t>(strip.width()) * strip.height() * 4);
    const float step = 1.0f / static_cast<float>(size - 1);
    for (uint32_t y = 0; y < strip.height(); ++y) {
        for (uint32_t x = 0; x < strip.width(); ++x) {
            uint16_t* texel = &strip.rgba[(static_cast<size_t>(y) * strip.width() + x) * 4];
            texel[0] = to_unorm16(static_cast<float>(x % size) * step);
            texel[1] = to_unorm16(static_cast<float>(y) * step);
            texel[2] = to_unorm16(static_cast<float>(x / size) * step);
            texel[3] = 65535;
        }
    }
    return strip;
}

Result<LutStrip> decode_lut_strip(std::span<const uint8_t> image_bytes) {
    const auto* data = image_bytes.data();
    const int len = static_cast<int>(image_bytes.size());
    int width = 0;
    int height = 0;
    int channels = 0;
    LutStrip strip;
    if (stbi_is_16_bit_from_memory(data, len) != 0) {
        stbi_us* pixels = stbi_load_16_from_memory(data, len, &width, &height, &channels, 4);
        if (pixels == nullptr) return make_error(std::string("unreadable image: ") + stbi_failure_reason());
        strip.rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
        stbi_image_free(pixels);
    } else {
        stbi_uc* pixels = stbi_load_from_memory(data, len, &width, &height, &channels, 4);
        if (pixels == nullptr) return make_error(std::string("unreadable image: ") + stbi_failure_reason());
        strip.rgba.resize(static_cast<size_t>(width) * height * 4);
        // 8 bits to 16 by repeating the byte, so 255 lands on 65535.
        for (size_t i = 0; i < strip.rgba.size(); ++i) {
            strip.rgba[i] = static_cast<uint16_t>(pixels[i] * 257);
        }
        stbi_image_free(pixels);
    }
    const auto size = static_cast<uint32_t>(height);
    if (size < MIN_SIZE || size > MAX_SIZE || static_cast<uint32_t>(width) != size * size) {
        return make_error("is " + std::to_string(width) + "x" + std::to_string(height)
                          + "; a LUT strip is N·N wide and N tall, like 256x16 or 1024x32");
    }
    strip.size = size;
    return strip;
}

Result<LutStrip> decode_cube(std::string_view text) {
    uint32_t cube_size = 0;
    std::array<float, 3> domain_min{0.0f, 0.0f, 0.0f};
    std::array<float, 3> domain_max{1.0f, 1.0f, 1.0f};
    std::vector<std::array<float, 3>> rows;

    std::istringstream lines{std::string(text)};
    std::string line;
    uint32_t line_number = 0;
    while (std::getline(lines, line)) {
        ++line_number;
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        std::istringstream words(line.substr(first));
        std::string key;
        words >> key;
        const auto located = [&](const std::string& what) {
            return make_error("line " + std::to_string(line_number) + ": " + what);
        };
        if (key == "TITLE") continue;
        if (key == "LUT_1D_SIZE") return located("a 1D table is not a colour grade; export a 3D .cube");
        if (key == "LUT_3D_SIZE") {
            if (!(words >> cube_size) || cube_size < MIN_SIZE || cube_size > 256) {
                return located("LUT_3D_SIZE must be a size from 2 to 256");
            }
            continue;
        }
        if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
            auto& domain = key == "DOMAIN_MIN" ? domain_min : domain_max;
            if (!(words >> domain[0] >> domain[1] >> domain[2])) return located(key + " needs three numbers");
            continue;
        }
        std::array<float, 3> row{};
        std::istringstream numbers(line.substr(first));
        if (!(numbers >> row[0] >> row[1] >> row[2])) {
            return located("'" + key + "' is neither a keyword nor a row of three numbers");
        }
        rows.push_back(row);
    }
    if (cube_size == 0) return make_error("no LUT_3D_SIZE");
    const size_t expected = static_cast<size_t>(cube_size) * cube_size * cube_size;
    if (rows.size() != expected) {
        return make_error("LUT_3D_SIZE " + std::to_string(cube_size) + " wants "
                          + std::to_string(expected) + " rows and there are "
                          + std::to_string(rows.size()));
    }
    for (int c = 0; c < 3; ++c) {
        if (!(domain_max[c] > domain_min[c])) return make_error("DOMAIN_MAX must be above DOMAIN_MIN");
    }

    // Red changes fastest through the rows, then green, then blue.
    const auto at = [&](uint32_t r, uint32_t g, uint32_t b) -> const std::array<float, 3>& {
        return rows[(static_cast<size_t>(b) * cube_size + g) * cube_size + r];
    };
    const auto sample = [&](std::array<float, 3> input) {
        std::array<uint32_t, 3> lo{};
        std::array<float, 3> t{};
        for (int c = 0; c < 3; ++c) {
            const float u = std::clamp((input[c] - domain_min[c]) / (domain_max[c] - domain_min[c]), 0.0f, 1.0f)
                          * static_cast<float>(cube_size - 1);
            lo[c] = std::min(static_cast<uint32_t>(u), cube_size - 2);
            t[c] = u - static_cast<float>(lo[c]);
        }
        std::array<float, 3> out{};
        for (int corner = 0; corner < 8; ++corner) {
            const uint32_t dr = corner & 1U;
            const uint32_t dg = (corner >> 1) & 1U;
            const uint32_t db = (corner >> 2) & 1U;
            const float w = (dr != 0 ? t[0] : 1.0f - t[0]) * (dg != 0 ? t[1] : 1.0f - t[1])
                          * (db != 0 ? t[2] : 1.0f - t[2]);
            const auto& v = at(lo[0] + dr, lo[1] + dg, lo[2] + db);
            for (int c = 0; c < 3; ++c) out[c] += v[c] * w;
        }
        return out;
    };

    LutStrip strip = neutral_lut(CUBE_STRIP_SIZE);
    const float step = 1.0f / static_cast<float>(strip.size - 1);
    for (uint32_t y = 0; y < strip.height(); ++y) {
        for (uint32_t x = 0; x < strip.width(); ++x) {
            const auto out = sample({static_cast<float>(x % strip.size) * step, static_cast<float>(y) * step,
                                     static_cast<float>(x / strip.size) * step});
            uint16_t* texel = &strip.rgba[(static_cast<size_t>(y) * strip.width() + x) * 4];
            for (int c = 0; c < 3; ++c) texel[c] = to_unorm16(out[c]);
        }
    }
    return strip;
}

Result<LutRecipe> read_lut_recipe(const fs::path& recipe_path) {
    const std::string where = recipe_path.string();
    const auto text = read_file(recipe_path);
    if (!text) return make_error(where + ": cannot be read");
    const auto j = nlohmann::json::parse(*text, nullptr, false);
    if (!j.is_object()) return make_error(where + ": is not a JSON object");
    const auto source = j.find("source");
    if (source == j.end() || !source->is_string() || source->get<std::string>().empty()) {
        return make_error(where + ": 'source' must name a strip image or a .cube");
    }
    LutRecipe recipe;
    recipe.source = source->get<std::string>();
    recipe.name = j.value("name", recipe_path.stem().string());
    return recipe;
}

fs::path lut_source_path(const fs::path& recipe_path, const LutRecipe& recipe) {
    return recipe_path.parent_path() / recipe.source;
}

Result<LutStrip> load_lut(const fs::path& recipe_path) {
    auto recipe = read_lut_recipe(recipe_path);
    if (!recipe) return make_error(recipe.error());
    const fs::path source = lut_source_path(recipe_path, *recipe);
    const auto bytes = read_file(source);
    if (!bytes) return make_error(recipe_path.string() + ": its source " + source.string() + " cannot be read");

    auto strip = source.extension() == ".cube"
        ? decode_cube(*bytes)
        : decode_lut_strip(std::span{reinterpret_cast<const uint8_t*>(bytes->data()), bytes->size()});
    if (!strip) return make_error(source.string() + ": " + strip.error());
    return strip;
}

std::vector<uint8_t> encode_lut_png(const LutStrip& strip) {
    // Scanlines of 16-bit RGB, big-endian as PNG wants, each behind a filter
    // byte of 0: no prediction, which a gradient table compresses well enough.
    const uint32_t width = strip.width();
    const uint32_t height = strip.height();
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 6));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        for (uint32_t x = 0; x < width; ++x) {
            const uint16_t* texel = &strip.rgba[(static_cast<size_t>(y) * width + x) * 4];
            for (int c = 0; c < 3; ++c) {
                raw.push_back(static_cast<uint8_t>(texel[c] >> 8));
                raw.push_back(static_cast<uint8_t>(texel[c] & 0xFF));
            }
        }
    }
    int compressed_len = 0;
    unsigned char* compressed = stbi_zlib_compress(raw.data(), static_cast<int>(raw.size()), &compressed_len, 8);

    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto put32 = [](std::vector<uint8_t>& out, uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
    };
    const auto crc32 = [](const uint8_t* bytes, size_t n) {
        uint32_t crc = 0xFFFFFFFFU;
        for (size_t i = 0; i < n; ++i) {
            crc ^= bytes[i];
            for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
        return crc ^ 0xFFFFFFFFU;
    };
    const auto chunk = [&](const char* type, const uint8_t* data, size_t n) {
        put32(png, static_cast<uint32_t>(n));
        const size_t start = png.size();
        png.insert(png.end(), type, type + 4);
        if (n > 0) png.insert(png.end(), data, data + n);
        put32(png, crc32(png.data() + start, n + 4));
    };

    std::vector<uint8_t> header;
    put32(header, width);
    put32(header, height);
    header.insert(header.end(), {16, 2, 0, 0, 0}); // 16 bits, RGB, deflate, no filter set, no interlace
    chunk("IHDR", header.data(), header.size());
    chunk("IDAT", compressed, static_cast<size_t>(compressed_len));
    chunk("IEND", nullptr, 0);
    std::free(compressed);
    return png;
}

} // namespace fjell
