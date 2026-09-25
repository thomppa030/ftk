#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <functional>

#include "glsl_shim.hpp"

// The post effects compiled as C++. The template's accessors become functions
// over an image each test describes as a function of uv, 64 by 64 texels, so a
// filter can be held to what it should do with a flat image, an edge, a
// silhouette or a crease.
namespace post_fx {

std::function<vec3(vec2)> scene;
std::function<float(vec2)> depth;
std::function<vec3(vec2)> normal;

constexpr float SIZE = 64.0f;

vec2 post_texel_size() { return vec2(1.0f / SIZE); }
vec3 scene_at(vec2 uv) { return scene(uv); }
float depth_at(vec2 uv) { return depth(uv); }
vec3 normal_at(vec2 uv) { return normal(uv); }

#ifdef _MSC_VER
#pragma warning(push)
// A GLSL literal is a double to C++, and the shader's float arithmetic
// narrows it back.
#pragma warning(disable : 4244 4305)
#endif
#include "../shaders/include/fjell_post_effects.glsl"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// The centre of texel (x, y).
vec2 texel(int x, int y) {
    return vec2((static_cast<float>(x) + 0.5f) / SIZE, (static_cast<float>(y) + 0.5f) / SIZE);
}

// Left half one value, right half another, split between texels 31 and 32.
template <typename T>
std::function<T(vec2)> split(T left, T right) {
    return [=](vec2 uv) { return uv.x < 0.5f ? left : right; };
}

void flat_image() {
    scene = [](vec2) { return vec3(2.5f, 1.0f, 0.3f); };
    depth = [](vec2) { return 12.0f; };
    normal = [](vec2) { return vec3(0.0f, 1.0f, 0.0f); };
}

bool near(vec3 a, vec3 b, float tolerance = 1e-5f) {
    return glm::all(glm::lessThanEqual(glm::abs(a - b), vec3(tolerance)));
}

} // namespace post_fx

using namespace post_fx;
using Catch::Approx;

TEST_CASE("Post effects give a flat image back as it was", "[post][effects]") {
    flat_image();
    const vec2 uv = texel(20, 40);
    const vec3 color = scene(uv);

    REQUIRE(near(sharpen(uv, 0.0f), color));
    REQUIRE(near(sharpen(uv, 0.5f), color));
    REQUIRE(near(sharpen(uv, 1.0f), color));
    REQUIRE(outline(uv, 0.1f, 0.3f) == 0.0f);
    for (int radius = 0; radius <= 3; ++radius) {
        INFO("radius " << radius);
        REQUIRE(near(kuwahara(uv, radius), color));
    }
    REQUIRE(near(chromatic_aberration(uv, 0.01f), color));
}

TEST_CASE("Sharpen pushes an edge apart and leaves amount 0 alone", "[post][effects]") {
    flat_image();
    scene = split(vec3(0.2f), vec3(0.8f));
    const vec2 dark = texel(31, 10);
    const vec2 bright = texel(32, 10);

    REQUIRE(near(sharpen(dark, 0.0f), vec3(0.2f)));
    const vec3 darker = sharpen(dark, 1.0f);
    const vec3 brighter = sharpen(bright, 1.0f);
    REQUIRE(darker.r < 0.2f);
    REQUIRE(darker.r > 0.0f);
    REQUIRE(brighter.r > 0.8f);
    // One pixel in from the edge, both neighbours match: nothing to sharpen.
    REQUIRE(near(sharpen(texel(30, 10), 1.0f), vec3(0.2f)));
}

TEST_CASE("An outline draws on the near side of a silhouette only", "[post][effects]") {
    flat_image();
    depth = split(5.0f, 50.0f);

    REQUIRE(outline(texel(31, 10), 0.1f, 0.0f) == Approx(1.0f));
    REQUIRE(outline(texel(32, 10), 0.1f, 0.0f) == 0.0f);
    REQUIRE(outline(texel(20, 10), 0.1f, 0.0f) == 0.0f);
    // The depth half off: the same silhouette draws nothing.
    REQUIRE(outline(texel(31, 10), 0.0f, 0.3f) == 0.0f);
}

TEST_CASE("A floor seen at a low angle draws no outline", "[post][effects]") {
    flat_image();
    // A plane: its inverse depth changes evenly down the screen, while the
    // depth itself runs from 2 m to 200 m.
    depth = [](vec2 uv) { return 1.0f / (0.5f - 0.495f * uv.y); };

    for (int y = 1; y < 63; y += 7) {
        INFO("row " << y);
        REQUIRE(outline(texel(20, y), 0.05f, 0.0f) == Approx(0.0f).margin(1e-4));
    }
}

TEST_CASE("A crease draws where the surface turns, and never against the sky", "[post][effects]") {
    flat_image();
    normal = split(vec3(0.0f, 1.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f));

    REQUIRE(outline(texel(31, 10), 0.0f, 0.3f) == Approx(1.0f));
    REQUIRE(outline(texel(32, 10), 0.0f, 0.3f) == Approx(1.0f));
    REQUIRE(outline(texel(20, 10), 0.0f, 0.3f) == 0.0f);

    normal = split(vec3(0.0f, 1.0f, 0.0f), vec3(0.0f));
    REQUIRE(outline(texel(31, 10), 0.0f, 0.3f) == 0.0f);
    REQUIRE(outline(texel(32, 10), 0.0f, 0.3f) == 0.0f);
}

TEST_CASE("Kuwahara keeps an edge rather than blurring across it", "[post][effects]") {
    flat_image();
    scene = split(vec3(0.1f), vec3(0.9f));

    // Next to the edge, a quarter lying wholly on the pixel's own side has no
    // variation at all, and wins.
    REQUIRE(near(kuwahara(texel(31, 10), 2), vec3(0.1f)));
    REQUIRE(near(kuwahara(texel(32, 10), 2), vec3(0.9f)));
}

TEST_CASE("Chromatic aberration pulls red and blue apart toward the edge", "[post][effects]") {
    flat_image();
    scene = [](vec2 uv) { return vec3(uv.x); };

    const vec3 centre = chromatic_aberration(vec2(0.5f), 0.01f);
    REQUIRE(near(centre, vec3(0.5f)));
    const vec3 edge = chromatic_aberration(vec2(1.0f, 0.5f), 0.01f);
    REQUIRE(edge.r == Approx(1.01f));
    REQUIRE(edge.g == Approx(1.0f));
    REQUIRE(edge.b == Approx(0.99f));
}

TEST_CASE("A vignette leaves the middle and darkens the corners by its amount", "[post][effects]") {
    flat_image();
    REQUIRE(vignette(vec2(0.5f), 0.6f, 0.5f) == Approx(1.0f));
    REQUIRE(vignette(vec2(0.0f), 0.6f, 0.5f) == Approx(0.4f));
    REQUIRE(vignette(vec2(1.0f, 0.0f), 0.6f, 0.5f) == Approx(0.4f));
    REQUIRE(vignette(vec2(0.55f, 0.5f), 0.6f, 0.5f) == Approx(1.0f));
}

TEST_CASE("Film grain stays within half its amount either way", "[post][effects]") {
    float lowest = 1.0f;
    float highest = -1.0f;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const float g = film_grain(vec2(static_cast<float>(x), static_cast<float>(y)) + vec2(0.5f),
                                       3.7f, 0.1f);
            lowest = std::min(lowest, g);
            highest = std::max(highest, g);
        }
    }
    REQUIRE(lowest >= -0.05f);
    REQUIRE(highest <= 0.05f);
    // Grain, not a constant.
    REQUIRE(highest - lowest > 0.08f);
}

TEST_CASE("Posterize bands by brightness and keeps the hue", "[post][effects]") {
    // 0.3 is a fifth of the way into the second of four bands: it drops to
    // that band's floor, 0.25, and the other channels scale with it.
    const vec3 low = posterize(vec3(0.3f, 0.15f, 0.0f), 4.0f);
    REQUIRE(near(low, vec3(0.25f, 0.125f, 0.0f)));
    // 0.45 is most of the way through it, and rises to the next, 0.5.
    const vec3 high = posterize(vec3(0.45f, 0.3f, 0.15f), 4.0f);
    REQUIRE(near(high, vec3(0.5f, 0.3f / 0.45f * 0.5f, 0.15f / 0.45f * 0.5f)));
    REQUIRE(near(posterize(vec3(0.0f), 4.0f), vec3(0.0f)));
}
