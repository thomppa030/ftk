#include "renderer/uniform_types.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <vector>

#include "glsl_shim.hpp"

// DDGI's shader math compiled as C++. The shader reads its constants as
// `ddgi` and the probe atlases through four ddgi_cage_* functions; here the
// constants are the engine's own GpuDdgiConstants and the atlases are
// functions each test describes, one value per probe.
namespace ddgi_shim {

fjell::GpuDdgiConstants ddgi{};

// Stand-ins for the atlases, one entry per probe. Irradiance is stored
// encoded, as the update pass writes it.
std::vector<vec3> stub_irradiance;
std::vector<vec2> stub_visibility;
std::vector<vec3> stub_offset;
std::vector<uint> stub_status;

// The probe whose atlas tile holds uv.
int probe_at(vec2 uv, int tex_w, int tex_h, int side) {
    const int x = static_cast<int>(std::floor(uv.x * static_cast<float>(tex_w)));
    const int y = static_cast<int>(std::floor(uv.y * static_cast<float>(tex_h)));
    const int tile = side + 2;
    return (x / tile) + (tex_w / tile) * (y / tile);
}

vec3 ddgi_cage_irradiance(vec2 uv) {
    return stub_irradiance[static_cast<size_t>(
        probe_at(uv, ddgi.irr_tex_w, ddgi.irr_tex_h, ddgi.irr_side))];
}
vec2 ddgi_cage_visibility(vec2 uv) {
    return stub_visibility[static_cast<size_t>(
        probe_at(uv, ddgi.vis_tex_w, ddgi.vis_tex_h, ddgi.vis_side))];
}
vec3 ddgi_cage_probe_offset(int probe_index) {
    return stub_offset[static_cast<size_t>(probe_index)];
}
uint ddgi_cage_probe_status(int probe_index) {
    return stub_status[static_cast<size_t>(probe_index)];
}

#ifdef _MSC_VER
#pragma warning(push)
// A GLSL literal is a double to C++, and the shader's float arithmetic
// narrows it back.
#pragma warning(disable : 4244 4305)
#endif
#define FJELL_DDGI_EXTERNAL_BINDINGS
#define FJELL_DDGI_CAGE
#include "../shaders/include/fjell_ddgi.glsl"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// An atlas laid out the way DdgiPass sizes one: tiles of side + 2 texels, as
// many to a row as the square root of the probe count rounded up.
struct AtlasSize {
    int width;
    int height;
};
AtlasSize atlas_size(int total_probes, int side) {
    const int per_row = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(total_probes))));
    const int tile = side + 2;
    return {per_row * tile, ((total_probes + per_row - 1) / per_row) * tile};
}

// A grid of `counts` probes `spacing` apart from `origin`, every probe active
// with the same irradiance, seeing nothing within `far` in any direction, and
// not relocated.
void uniform_field(ivec3 counts, vec3 spacing, vec3 origin, vec3 irradiance, float far) {
    ddgi = {};
    ddgi.probe_grid_position = origin;
    ddgi.probe_spacing = spacing;
    ddgi.reciprocal_spacing = vec3(1.0f) / spacing;
    ddgi.probe_counts = counts;
    ddgi.irr_side = 8;
    ddgi.vis_side = 6;
    const int total = counts.x * counts.y * counts.z;
    const AtlasSize irr = atlas_size(total, ddgi.irr_side);
    const AtlasSize vis = atlas_size(total, ddgi.vis_side);
    ddgi.irr_tex_w = irr.width;
    ddgi.irr_tex_h = irr.height;
    ddgi.vis_tex_w = vis.width;
    ddgi.vis_tex_h = vis.height;
    ddgi.normal_bias = 0.25f;
    ddgi.view_bias = 0.1f;
    ddgi.camera_position = origin + vec3(counts) * spacing * 0.5f + vec3(0.3f, 5.0f, 0.7f);

    const auto n = static_cast<size_t>(total);
    stub_irradiance.assign(n, ddgi_encode_irradiance(irradiance));
    stub_visibility.assign(n, vec2(far, far * far));
    stub_offset.assign(n, vec3(0.0f));
    stub_status.assign(n, PROBE_STATUS_ACTIVE);
}

bool near(vec3 a, vec3 b, float tolerance) {
    return glm::all(glm::lessThanEqual(glm::abs(a - b), vec3(tolerance)));
}

} // namespace ddgi_shim

using namespace ddgi_shim;
using Catch::Approx;

TEST_CASE("Irradiance encoding round-trips up to its ceiling and clamps past it", "[ddgi]") {
    for (float v : {0.0f, 0.001f, 0.05f, 0.5f, 1.0f, 3.7f, 12.0f, 29.5f}) {
        INFO("value " << v);
        const vec3 back = ddgi_decode_irradiance(ddgi_encode_irradiance(vec3(v)));
        REQUIRE(back.x == Approx(v).margin(1e-6).epsilon(1e-4));
    }
    REQUIRE(ddgi_decode_irradiance(ddgi_encode_irradiance(vec3(80.0f))).x
            == Approx(IRRADIANCE_ENCODE_CEILING).epsilon(1e-4));
    REQUIRE(ddgi_decode_irradiance(ddgi_encode_irradiance(vec3(-2.0f))).x == 0.0f);
}

TEST_CASE("Spherical Fibonacci gives unit directions spread over the sphere", "[ddgi]") {
    constexpr int RAYS = 128;
    vec3 sum(0.0f);
    int upper = 0;
    for (int i = 0; i < RAYS; ++i) {
        const vec3 d = spherical_fibonacci(static_cast<float>(i), static_cast<float>(RAYS));
        REQUIRE(glm::length(d) == Approx(1.0f).epsilon(1e-5));
        sum += d;
        if (d.z > 0.0f) ++upper;
    }
    REQUIRE(upper == RAYS / 2);
    REQUIRE(glm::length(sum) / static_cast<float>(RAYS) < 0.02f);
}

TEST_CASE("Octahedral encode and decode are inverses", "[ddgi]") {
    constexpr int N = 500;
    for (int i = 0; i < N; ++i) {
        const vec3 d = spherical_fibonacci(static_cast<float>(i), static_cast<float>(N));
        INFO("direction " << d.x << ", " << d.y << ", " << d.z);
        REQUIRE(near(oct_decode(oct_encode(d)), d, 1e-5f));
    }
}

TEST_CASE("Probe indices and grid coordinates convert both ways", "[ddgi]") {
    for (ivec3 counts : {ivec3(16, 16, 16), ivec3(18, 9, 12)}) {
        uniform_field(counts, vec3(2.0f), vec3(0.0f), vec3(1.0f), 100.0f);
        const int total = counts.x * counts.y * counts.z;
        for (int i = 0; i < total; ++i) {
            const ivec3 g = ddgi_probe_index_to_grid(i);
            REQUIRE(glm::all(glm::greaterThanEqual(g, ivec3(0))));
            REQUIRE(glm::all(glm::lessThan(g, counts)));
            REQUIRE(ddgi_grid_to_probe_index(g) == i);

            // One texel of the probe offset image each, inside its
            // probes_xy x probes_z extent.
            const ivec2 t = ddgi_probe_offset_texel(i);
            REQUIRE(t.x == i % (counts.x * counts.y));
            REQUIRE(t.y == i / (counts.x * counts.y));
            REQUIRE(t.y < counts.z);
        }
    }
}

TEST_CASE("World positions pick their cell and clamp to the grid's edge", "[ddgi]") {
    uniform_field(ivec3(16, 16, 16), vec3(2.0f, 1.0f, 2.0f), vec3(-10.0f, -2.0f, -10.0f),
                  vec3(1.0f), 100.0f);
    REQUIRE(ddgi_world_to_grid(vec3(-10.0f, -2.0f, -10.0f)) == ivec3(0));
    REQUIRE(ddgi_world_to_grid(vec3(-5.1f, 0.5f, 3.9f)) == ivec3(2, 2, 6));
    REQUIRE(ddgi_world_to_grid(vec3(-500.0f, -500.0f, -500.0f)) == ivec3(0));
    REQUIRE(ddgi_world_to_grid(vec3(500.0f, 500.0f, 500.0f)) == ivec3(15));
}

TEST_CASE("The probe field covers the grid and hands over to the sky across its last cell",
          "[ddgi]") {
    // Four probes a side, 2 m apart on X and Z and 1 m on Y: the grid spans
    // 6 m x 3 m x 6 m from the origin.
    uniform_field(ivec3(4), vec3(2.0f, 1.0f, 2.0f), vec3(0.0f), vec3(1.0f), 100.0f);

    // Well inside, the field gives all of it.
    CHECK(ddgi_grid_coverage(vec3(3.0f, 1.5f, 3.0f)) == 1.0f);
    // Halfway through the outermost cell on X, half.
    CHECK(ddgi_grid_coverage(vec3(1.0f, 1.5f, 3.0f)) == Catch::Approx(0.5f));
    // The same distance in from the Y face is a whole cell there.
    CHECK(ddgi_grid_coverage(vec3(3.0f, 2.0f, 3.0f)) == 1.0f);
    CHECK(ddgi_grid_coverage(vec3(3.0f, 2.75f, 3.0f)) == Catch::Approx(0.25f));
    // On the outer probes and past them, none: the sky's IBL instead.
    CHECK(ddgi_grid_coverage(vec3(6.0f, 1.5f, 3.0f)) == 0.0f);
    CHECK(ddgi_grid_coverage(vec3(-4.0f, 1.5f, 3.0f)) == 0.0f);
    CHECK(ddgi_grid_depth(vec3(-4.0f, 1.5f, 3.0f)) == Catch::Approx(-2.0f));
}

// The update pass writes a texel from the direction normalized_oct_coord()
// gives it; the cage reads a direction through get_probe_uv(). The two only
// agree if every texel's direction samples back into its own centre.
TEST_CASE("Every atlas texel's direction samples back into that texel", "[ddgi]") {
    constexpr int PROBES = 10;
    for (int side : {6, 8}) {
        const AtlasSize atlas = atlas_size(PROBES, side);
        const int tile = side + 2;
        const int per_row = atlas.width / tile;
        for (int p = 0; p < PROBES; ++p) {
            const ivec2 origin((p % per_row) * tile, (p / per_row) * tile);
            for (int y = 1; y <= side; ++y) {
                for (int x = 1; x <= side; ++x) {
                    const ivec2 texel = origin + ivec2(x, y);
                    INFO("side " << side << " probe " << p << " texel " << x << "," << y);
                    REQUIRE(get_probe_index_from_pixels(texel, tile, atlas.width) == p);

                    const vec3 dir = oct_decode(normalized_oct_coord(texel, side));
                    const vec2 uv = get_probe_uv(dir, p, atlas.width, atlas.height, side);
                    const vec2 at = uv * vec2(static_cast<float>(atlas.width),
                                              static_cast<float>(atlas.height));
                    REQUIRE(at.x == Approx(static_cast<float>(texel.x) + 0.5f).margin(1e-3));
                    REQUIRE(at.y == Approx(static_cast<float>(texel.y) + 0.5f).margin(1e-3));
                }
            }
        }
    }
}

// A ring texel's centre lies just past an edge of the octahedral map. Along an
// edge the map meets itself read backwards, so folding the centre back across
// the edge it overhangs (across both at a corner) lands on the centre of the
// interior texel whose value the ring texel must hold.
TEST_CASE("Every border texel copies the interior texel the octahedron folds it onto", "[ddgi]") {
    for (int side : {6, 8}) {
        const int last = side + 1;
        const float s = static_cast<float>(side);
        auto to_oct = [s](int t) { return (static_cast<float>(t - 1) + 0.5f) * 2.0f / s - 1.0f; };
        auto to_texel = [s](float o) {
            return static_cast<int>(std::lround((o + 1.0f) * s / 2.0f - 0.5f)) + 1;
        };
        for (int y = 0; y <= last; ++y) {
            for (int x = 0; x <= last; ++x) {
                if (x != 0 && y != 0 && x != last && y != last) continue;
                vec2 o(to_oct(x), to_oct(y));
                if (o.x > 1.0f) o = vec2(2.0f - o.x, -o.y);
                else if (o.x < -1.0f) o = vec2(-2.0f - o.x, -o.y);
                if (o.y > 1.0f) o = vec2(-o.x, 2.0f - o.y);
                else if (o.y < -1.0f) o = vec2(-o.x, -2.0f - o.y);

                INFO("side " << side << " ring texel " << x << "," << y);
                const ivec2 source = ddgi_border_source(ivec2(x, y), side);
                REQUIRE(source.x == to_texel(o.x));
                REQUIRE(source.y == to_texel(o.y));
                REQUIRE(source.x >= 1);
                REQUIRE(source.x <= side);
                REQUIRE(source.y >= 1);
                REQUIRE(source.y <= side);
            }
        }
    }
}

namespace {

// Points strictly inside the grid's cells, away from any probe, with the
// normals a floor, a ceiling and two walls have.
const vec3 SAMPLE_POINTS[] = {
    vec3(1.3f, 2.6f, 3.1f), vec3(4.9f, 0.4f, 1.7f), vec3(2.2f, 5.3f, 4.4f), vec3(0.7f, 1.1f, 5.6f),
};
const vec3 SAMPLE_NORMALS[] = {
    vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, -1.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f),
    glm::normalize(vec3(-0.3f, 0.2f, 0.9f)),
};

} // namespace

TEST_CASE("A uniform probe field gives its irradiance everywhere inside it", "[ddgi]") {
    const vec3 light(0.3f, 0.6f, 0.9f);
    uniform_field(ivec3(4), vec3(2.0f), vec3(0.0f), light, 100.0f);
    for (const vec3& p : SAMPLE_POINTS) {
        for (const vec3& n : SAMPLE_NORMALS) {
            const DdgiCageSample s = ddgi_sample_cage(p, n, ddgi.camera_position, false);
            INFO("point " << p.x << "," << p.y << "," << p.z
                 << " normal " << n.x << "," << n.y << "," << n.z);
            REQUIRE(s.sum_weight > 0.0f);
            REQUIRE(s.cheb_tested == 0.0f);
            REQUIRE(near(s.irradiance, light * DDGI_ENERGY_CONSERVATION, 1e-4f));
        }
    }
}

TEST_CASE("A probe the status pass switched off does not reach the cage", "[ddgi]") {
    const vec3 light(0.3f, 0.6f, 0.9f);
    uniform_field(ivec3(4), vec3(2.0f), vec3(0.0f), light, 100.0f);
    const vec3 p = SAMPLE_POINTS[0];
    const int corner = ddgi_grid_to_probe_index(ddgi_world_to_grid(p));
    stub_irradiance[static_cast<size_t>(corner)] = ddgi_encode_irradiance(vec3(30.0f, 0.0f, 0.0f));

    stub_status[static_cast<size_t>(corner)] = PROBE_STATUS_OFF;
    REQUIRE(near(ddgi_sample_cage(p, SAMPLE_NORMALS[0], ddgi.camera_position, false).irradiance,
                 light * DDGI_ENERGY_CONSERVATION, 1e-4f));

    // Switched back on, the same probe does pull the result toward red.
    stub_status[static_cast<size_t>(corner)] = PROBE_STATUS_ACTIVE;
    REQUIRE(ddgi_sample_cage(p, SAMPLE_NORMALS[0], ddgi.camera_position, false).irradiance.x
            > light.x * DDGI_ENERGY_CONSERVATION + 0.1f);
}

TEST_CASE("A probe not traced since the last reset does not reach the cage", "[ddgi]") {
    const vec3 light(0.3f, 0.6f, 0.9f);
    uniform_field(ivec3(4), vec3(2.0f), vec3(0.0f), light, 100.0f);
    const vec3 p = SAMPLE_POINTS[2];
    const int corner = ddgi_grid_to_probe_index(ddgi_world_to_grid(p));
    // What a reset leaves in its atlases: nothing.
    stub_irradiance[static_cast<size_t>(corner)] = vec3(0.0f);
    stub_visibility[static_cast<size_t>(corner)] = vec2(0.0f);
    stub_status[static_cast<size_t>(corner)] = PROBE_STATUS_UNINITIALIZED;

    const DdgiCageSample s = ddgi_sample_cage(p, SAMPLE_NORMALS[3], ddgi.camera_position, false);
    REQUIRE(s.cheb_tested == 0.0f);
    REQUIRE(near(s.irradiance, light * DDGI_ENERGY_CONSERVATION, 1e-4f));
}

TEST_CASE("With no cage probe traced since the last reset the cage gives no light", "[ddgi]") {
    uniform_field(ivec3(4), vec3(2.0f), vec3(0.0f), vec3(1.0f), 100.0f);
    stub_status.assign(stub_status.size(), PROBE_STATUS_UNINITIALIZED);
    const DdgiCageSample s =
        ddgi_sample_cage(SAMPLE_POINTS[3], SAMPLE_NORMALS[1], ddgi.camera_position, false);
    REQUIRE(s.sum_weight == 0.0f);
    REQUIRE(s.irradiance == vec3(0.0f));
}

TEST_CASE("With every cage probe switched off the cage gives no light", "[ddgi]") {
    uniform_field(ivec3(4), vec3(2.0f), vec3(0.0f), vec3(1.0f), 100.0f);
    stub_status.assign(stub_status.size(), PROBE_STATUS_OFF);
    const DdgiCageSample s =
        ddgi_sample_cage(SAMPLE_POINTS[1], SAMPLE_NORMALS[0], ddgi.camera_position, false);
    REQUIRE(s.sum_weight == 0.0f);
    REQUIRE(s.irradiance == vec3(0.0f));
}

TEST_CASE("A probe whose recorded depth stops short of the point is weighted down", "[ddgi]") {
    uniform_field(ivec3(4), vec3(2.0f), vec3(0.0f), vec3(1.0f), 100.0f);
    const vec3 p = SAMPLE_POINTS[0];
    const int corner = ddgi_grid_to_probe_index(ddgi_world_to_grid(p));
    // Something 5 cm from the probe in every direction: the point, over a
    // metre away, is behind it.
    stub_visibility[static_cast<size_t>(corner)] = vec2(0.05f, 0.05f * 0.05f);

    const DdgiCageSample s = ddgi_sample_cage(p, SAMPLE_NORMALS[0], ddgi.camera_position, false);
    REQUIRE(s.cheb_tested == 1.0f);
    REQUIRE(s.cheb_floored == 1.0f);

    // The diagnostic that skips the test leaves it alone.
    REQUIRE(ddgi_sample_cage(p, SAMPLE_NORMALS[0], ddgi.camera_position, true).cheb_tested == 0.0f);
}
