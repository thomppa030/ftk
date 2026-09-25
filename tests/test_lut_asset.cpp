#include "core/math/color_space.hpp"
#include "renderer/resources/lut_asset.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// stb_image_write, for the 8-bit and misshapen strips the tests write.
// Declared rather than included: its header brings <stdlib.h>, whose global
// abs() collides with the GLSL shim's.
extern "C" int stbi_write_png(char const* filename, int w, int h, int comp, const void* data,
                              int stride_in_bytes);

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>

// The shader half, compiled as C++ against a sampler that filters the way the
// GPU does. See glsl_shim.hpp. The colour transfer is the engine's own CPU
// one: GLM will not multiply a vector by a double literal the way GLSL does,
// so fjell_color_space.glsl stays out and what is checked here is the table
// lookup, which is what lut_sample() adds.
#include "glsl_shim.hpp"

namespace lut_shim {

using glm::floor;
using glm::mix;

struct sampler2D {
    const fjell::LutStrip* strip;
};

glm::ivec2 textureSize(const sampler2D& s, int /*lod*/) {
    return {static_cast<int>(s.strip->width()), static_cast<int>(s.strip->height())};
}

// Bilinear, clamped to the edge, on the texel centres: what the post pass's
// linear clamp sampler does to a one-mip R16G16B16A16_UNORM image.
vec4 textureLod(const sampler2D& s, vec2 uv, float /*lod*/) {
    const auto& strip = *s.strip;
    const float x = uv.x * static_cast<float>(strip.width()) - 0.5f;
    const float y = uv.y * static_cast<float>(strip.height()) - 0.5f;
    const auto texel = [&](int tx, int ty) {
        tx = std::clamp(tx, 0, static_cast<int>(strip.width()) - 1);
        ty = std::clamp(ty, 0, static_cast<int>(strip.height()) - 1);
        const uint16_t* p = &strip.rgba[(static_cast<size_t>(ty) * strip.width() + static_cast<size_t>(tx)) * 4];
        return vec4(p[0], p[1], p[2], p[3]) / 65535.0f;
    };
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    return glm::mix(glm::mix(texel(x0, y0), texel(x0 + 1, y0), fx),
                    glm::mix(texel(x0, y0 + 1), texel(x0 + 1, y0 + 1), fx), fy);
}

#define FJELL_COLOR_SPACE_GLSL
vec3 linear_to_srgb(vec3 c) { return fjell::color_space::linear_to_srgb(c); }
vec3 srgb_to_linear(vec3 c) { return fjell::color_space::srgb_to_linear(c); }

#include "../shaders/include/fjell_lut.glsl"

} // namespace lut_shim

using namespace fjell;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

const uint16_t* texel_at(const LutStrip& strip, uint32_t x, uint32_t y) {
    return &strip.rgba[(static_cast<size_t>(y) * strip.width() + x) * 4];
}

// A strip filled from a grade function of the sRGB input, the way a .cube of
// the same grade is resampled onto one.
LutStrip strip_of(uint32_t size, const std::function<glm::vec3(glm::vec3)>& grade) {
    LutStrip strip = neutral_lut(size);
    const float step = 1.0f / static_cast<float>(size - 1);
    for (uint32_t y = 0; y < strip.height(); ++y) {
        for (uint32_t x = 0; x < strip.width(); ++x) {
            const glm::vec3 out = grade({static_cast<float>(x % size) * step, static_cast<float>(y) * step,
                                         static_cast<float>(x / size) * step});
            auto* t = &strip.rgba[(static_cast<size_t>(y) * strip.width() + x) * 4];
            for (int c = 0; c < 3; ++c) {
                t[c] = static_cast<uint16_t>(std::lround(std::clamp(out[c], 0.0f, 1.0f) * 65535.0f));
            }
        }
    }
    return strip;
}

std::string cube_of(uint32_t size, const std::function<glm::vec3(glm::vec3)>& grade,
                    const std::string& domain = {}) {
    std::ostringstream out;
    out << "# written by a test\nTITLE \"test\"\nLUT_3D_SIZE " << size << "\n" << domain;
    const float step = 1.0f / static_cast<float>(size - 1);
    for (uint32_t b = 0; b < size; ++b) {
        for (uint32_t g = 0; g < size; ++g) {
            for (uint32_t r = 0; r < size; ++r) {
                const glm::vec3 v = grade({static_cast<float>(r) * step, static_cast<float>(g) * step,
                                           static_cast<float>(b) * step});
                out << v.r << " " << v.g << " " << v.b << "\n";
            }
        }
    }
    return out.str();
}

// A grade that is linear in each channel, so a trilinear resample of it is
// exact and any difference is the decoder's.
glm::vec3 warm(glm::vec3 c) {
    return {0.8f * c.r + 0.15f, 0.6f * c.g + 0.2f * c.b + 0.1f, 0.9f - 0.7f * c.b};
}

fs::path scratch_dir() {
    const fs::path dir = fs::temp_directory_path() / "fjell-lut-test";
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

} // namespace

TEST_CASE("The neutral strip maps every grid colour to itself", "[lut]") {
    const LutStrip strip = neutral_lut(16);
    REQUIRE(strip.width() == 256);
    REQUIRE(strip.height() == 16);
    REQUIRE(strip.rgba.size() == 256 * 16 * 4);
    // First texel black, last white; a texel's blue is its slice.
    REQUIRE(texel_at(strip, 0, 0)[0] == 0);
    REQUIRE(texel_at(strip, 255, 15)[0] == 65535);
    REQUIRE(texel_at(strip, 255, 15)[1] == 65535);
    REQUIRE(texel_at(strip, 255, 15)[2] == 65535);
    REQUIRE(texel_at(strip, 16 * 5 + 3, 7)[0] == static_cast<uint16_t>(std::lround(3.0 / 15.0 * 65535.0)));
    REQUIRE(texel_at(strip, 16 * 5 + 3, 7)[1] == static_cast<uint16_t>(std::lround(7.0 / 15.0 * 65535.0)));
    REQUIRE(texel_at(strip, 16 * 5 + 3, 7)[2] == static_cast<uint16_t>(std::lround(5.0 / 15.0 * 65535.0)));
}

TEST_CASE("A strip written as a 16-bit PNG reads back unchanged", "[lut]") {
    for (uint32_t size : {16U, 32U}) {
        const LutStrip strip = strip_of(size, warm);
        const std::vector<uint8_t> png = encode_lut_png(strip);
        auto back = decode_lut_strip(png);
        REQUIRE(back);
        REQUIRE(back->size == size);
        REQUIRE(back->rgba == strip.rgba);
    }
}

TEST_CASE("An 8-bit strip is read, a strip of the wrong shape is refused", "[lut]") {
    const fs::path dir = scratch_dir();
    // 8 bits a channel: 255 has to land on 65535, not 65280.
    std::vector<uint8_t> pixels(256 * 16 * 4, 255);
    REQUIRE(stbi_write_png((dir / "eight.png").string().c_str(), 256, 16, 4, pixels.data(), 256 * 4) != 0);
    std::ifstream in(dir / "eight.png", std::ios::binary);
    const std::vector<uint8_t> eight((std::istreambuf_iterator<char>(in)), {});
    auto strip = decode_lut_strip(eight);
    REQUIRE(strip);
    REQUIRE(strip->size == 16);
    REQUIRE(strip->rgba[0] == 65535);

    std::vector<uint8_t> odd(20 * 10 * 4, 0);
    REQUIRE(stbi_write_png((dir / "odd.png").string().c_str(), 20, 10, 4, odd.data(), 20 * 4) != 0);
    std::ifstream odd_in(dir / "odd.png", std::ios::binary);
    const std::vector<uint8_t> odd_bytes((std::istreambuf_iterator<char>(odd_in)), {});
    auto refused = decode_lut_strip(odd_bytes);
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error().find("20x10") != std::string::npos);
}

TEST_CASE("A .cube and a strip of the same grade match", "[lut]") {
    auto cube = decode_cube(cube_of(33, warm));
    REQUIRE(cube);
    REQUIRE(cube->size == 32);
    const LutStrip strip = strip_of(32, warm);
    for (size_t i = 0; i < strip.rgba.size(); ++i) {
        INFO("texel channel " << i);
        REQUIRE(std::abs(static_cast<int>(cube->rgba[i]) - static_cast<int>(strip.rgba[i])) <= 4);
    }
}

TEST_CASE("A .cube's domain places its rows", "[lut]") {
    // A table over 0..2 is half used by colours that stop at 1: input 1
    // reads the table's middle, which for this grade is 1 as well.
    const auto identity_over_two = [](glm::vec3 c) { return c * 2.0f; };
    auto cube = decode_cube(cube_of(17, identity_over_two, "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n"));
    REQUIRE(cube);
    const uint16_t* last = texel_at(*cube, cube->width() - 1, cube->height() - 1);
    REQUIRE(last[0] == Approx(65535).margin(4));
    const uint16_t* first = texel_at(*cube, 0, 0);
    REQUIRE(first[0] == 0);
}

TEST_CASE("A broken .cube says where", "[lut]") {
    auto no_size = decode_cube("0 0 0\n");
    REQUIRE_FALSE(no_size);
    auto short_table = decode_cube("LUT_3D_SIZE 2\n0 0 0\n1 1 1\n");
    REQUIRE_FALSE(short_table);
    REQUIRE(short_table.error().find("8 rows") != std::string::npos);
    auto one_d = decode_cube("LUT_1D_SIZE 16\n");
    REQUIRE_FALSE(one_d);
    REQUIRE(one_d.error().find("line 1") != std::string::npos);
    auto junk = decode_cube("LUT_3D_SIZE 2\nhello there friend\n");
    REQUIRE_FALSE(junk);
    REQUIRE(junk.error().find("line 2") != std::string::npos);
}

TEST_CASE("A .fjlut recipe loads its source beside it", "[lut]") {
    const fs::path dir = scratch_dir();
    const auto png = encode_lut_png(strip_of(16, warm));
    write_file(dir / "warm.png", std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
    write_file(dir / "warm.fjlut", R"({"name": "Warm", "source": "warm.png"})");
    write_file(dir / "warm_cube.cube", cube_of(17, warm));
    write_file(dir / "warm_cube.fjlut", R"({"source": "warm_cube.cube"})");

    auto recipe = read_lut_recipe(dir / "warm.fjlut");
    REQUIRE(recipe);
    REQUIRE(recipe->name == "Warm");
    REQUIRE(read_lut_recipe(dir / "warm_cube.fjlut")->name == "warm_cube");

    auto strip = load_lut(dir / "warm.fjlut");
    REQUIRE(strip);
    REQUIRE(strip->rgba == strip_of(16, warm).rgba);
    auto from_cube = load_lut(dir / "warm_cube.fjlut");
    REQUIRE(from_cube);
    REQUIRE(from_cube->size == 32);

    write_file(dir / "lost.fjlut", R"({"source": "nowhere.png"})");
    auto lost = load_lut(dir / "lost.fjlut");
    REQUIRE_FALSE(lost);
    REQUIRE(lost.error().find("nowhere.png") != std::string::npos);
    write_file(dir / "bare.fjlut", R"({"name": "No source"})");
    REQUIRE_FALSE(load_lut(dir / "bare.fjlut"));
}

TEST_CASE("lut_sample through a neutral strip gives the colour back", "[lut][glsl]") {
    for (uint32_t size : {16U, 32U}) {
        const LutStrip strip = neutral_lut(size);
        const lut_shim::sampler2D sampler{&strip};
        INFO("strip " << size);
        // Across the whole cube, between grid points as well as on them. The
        // check is in sRGB, where one 8-bit output step is 1/255.
        for (int r = 0; r <= 20; ++r) {
            for (int g = 0; g <= 20; ++g) {
                for (int b = 0; b <= 20; ++b) {
                    const glm::vec3 srgb{static_cast<float>(r) / 20.0f, static_cast<float>(g) / 20.0f,
                                         static_cast<float>(b) / 20.0f};
                    const glm::vec3 linear = color_space::srgb_to_linear(srgb);
                    const glm::vec3 out = color_space::linear_to_srgb(lut_shim::lut_sample(sampler, linear));
                    INFO("sRGB " << srgb.r << " " << srgb.g << " " << srgb.b);
                    REQUIRE(glm::all(glm::lessThanEqual(glm::abs(out - srgb), glm::vec3(0.5f / 255.0f))));
                }
            }
        }
    }
}

TEST_CASE("lut_sample reads a grade between its grid points", "[lut][glsl]") {
    // A grade linear in each channel is exact under the lookup's bilinear
    // taps and the slice mix, so the sample matches the function itself.
    const LutStrip strip = strip_of(16, warm);
    const lut_shim::sampler2D sampler{&strip};
    for (const glm::vec3 srgb : {glm::vec3(0.1f, 0.5f, 0.9f), glm::vec3(0.73f, 0.22f, 0.41f),
                                 glm::vec3(1.0f, 0.0f, 0.5f)}) {
        const glm::vec3 out = color_space::linear_to_srgb(
            lut_shim::lut_sample(sampler, color_space::srgb_to_linear(srgb)));
        const glm::vec3 want = warm(srgb);
        REQUIRE(glm::all(glm::lessThanEqual(glm::abs(out - want), glm::vec3(1.0f / 255.0f))));
    }
}
