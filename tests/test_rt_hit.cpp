// The ray tracers' hit code, fjell_rt_hit.glsl, compiled as C++ so its rules
// can be checked on the CPU: which hits it drops, and how it reads the
// triangle, UV, normal, albedo and emission of the ones it keeps. The three
// tracers (DDGI, RT reflections, SS-GI) all light their hits through it.
// See glsl_shim.hpp.

#include "glsl_shim.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <iterator>
#include <vector>

// In a namespace of this file's own, like the other shader shims.
namespace rt_hit_shim {

#include "../shaders/include/fjell_gpu_types.glsl"

/// A storage buffer's runtime array. A read past its end throws where the
/// GPU would lose the device.
template <typename T>
struct ShaderArray {
    std::vector<T> items;
    T operator[](uint i) const { return items.at(i); }
    [[nodiscard]] uint length() const { return static_cast<uint>(items.size()); }
};

ShaderArray<float> vertex_data;
ShaderArray<uint> index_data;
ShaderArray<RtInstanceData> rt_instances;
ShaderArray<GpuMaterial> materials;

/// A bindless texture slot. Every read of any slot returns `texel` and is
/// recorded, so a test sees which slot was read, where and at which mip.
struct Sampler {
    uint slot;
};
struct TextureTable {
    Sampler operator[](uint slot) const { return {slot}; }
};
TextureTable textures;

struct TextureRead {
    uint slot;
    vec2 uv;
    float lod;
};
std::vector<TextureRead> reads;
vec4 texel{0.5f, 0.25f, 0.125f, 1.0f};

vec4 textureLod(Sampler s, vec2 uv, float lod) {
    reads.push_back({s.slot, uv, lod});
    return texel;
}

#include "../shaders/include/fjell_rt_hit.glsl"

} // namespace rt_hit_shim

using namespace rt_hit_shim;
using Catch::Approx;

namespace {

void push_vertex(vec3 position, vec2 uv, vec3 normal) {
    const float floats[15] = {position.x, position.y, position.z,
                              1.0f, 1.0f, 1.0f,
                              uv.x, uv.y,
                              normal.x, normal.y, normal.z,
                              1.0f, 0.0f, 0.0f, 1.0f};
    vertex_data.items.insert(vertex_data.items.end(), std::begin(floats), std::end(floats));
}

/// Two instances with a triangle each, sharing one index list the way the
/// mega buffer does. Instance 1 is the one hit: its triangle starts at index
/// 3 and its vertices at vertex 3, and it uses material 1.
void build_scene() {
    vertex_data.items.clear();
    for (int i = 0; i < 3; ++i) {
        push_vertex(vec3(0.0f), vec2(9.0f), vec3(0.0f, 1.0f, 0.0f));
    }
    push_vertex({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    push_vertex({1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    push_vertex({0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f});

    index_data.items = {0, 1, 2, 0, 1, 2};
    rt_instances.items = {RtInstanceData{0, 0, 0, 0}, RtInstanceData{1, 3, 3, 0}};

    GpuMaterial unused{};
    GpuMaterial surface{};
    surface.base_params[0] = 0.8f;   // base colour factor
    surface.base_params[1] = 0.6f;
    surface.base_params[2] = 0.4f;
    surface.base_params[7] = 2.0f;   // UV scale x
    surface.base_params[8] = 0.0f;   // UV scale y, unset
    surface.base_params[9] = 1.0f;   // emission factor
    surface.base_params[10] = 2.0f;
    surface.base_params[11] = 3.0f;
    surface.texture_indices[0] = 7;  // base colour
    surface.texture_indices[3] = 9;  // emissive
    surface.has_emissive_texture = 1;
    materials.items = {unused, surface};

    reads.clear();
}

/// A hit on `instance` placed in the world by `object_to_world`, reported
/// as the query reports it: through the inverse.
[[nodiscard]] RtHit hit_on(uint instance, uint primitive, vec2 barycentrics,
                           const mat4& object_to_world = mat4(1.0f)) {
    RtHit hit{};
    hit.instance = instance;
    hit.primitive = primitive;
    hit.barycentrics = barycentrics;
    hit.t = 1.0f;
    hit.world_to_object = mat4x3(glm::inverse(object_to_world));
    return hit;
}

/// Whether the hit is kept, failing the test if deciding read past a buffer.
[[nodiscard]] bool kept(const RtHit& hit) {
    bool in_bounds = false;
    REQUIRE_NOTHROW(in_bounds = rt_hit_in_bounds(hit));
    return in_bounds;
}

} // namespace

TEST_CASE("A hit inside every buffer resolves to its own triangle and material", "[rt_hit]") {
    build_scene();
    const RtHit hit = hit_on(1, 0, {0.25f, 0.5f});

    REQUIRE(kept(hit));
    const RtSurface surface = rt_resolve_hit(hit);
    CHECK(surface.vertices == uvec3(3, 4, 5));
    CHECK(surface.weights.x == Approx(0.25f));
    CHECK(surface.weights.y == Approx(0.25f));
    CHECK(surface.weights.z == Approx(0.5f));
    CHECK(surface.material.base_params[0] == Approx(0.8f));
}

TEST_CASE("A hit whose reads would leave a buffer is dropped without reading", "[rt_hit]") {
    build_scene();

    SECTION("an instance past the per-instance table") {
        CHECK_FALSE(kept(hit_on(2, 0, {0.0f, 0.0f})));
    }
    SECTION("a material past the material table") {
        rt_instances.items[1].material_id = 2;
        CHECK_FALSE(kept(hit_on(1, 0, {0.0f, 0.0f})));
    }
    SECTION("a triangle past the index buffer") {
        CHECK_FALSE(kept(hit_on(1, 1, {0.0f, 0.0f})));
    }
    SECTION("a vertex past the vertex buffer") {
        rt_instances.items[1].vertex_offset = 4;
        CHECK_FALSE(kept(hit_on(1, 0, {0.0f, 0.0f})));
    }
    SECTION("a triangle ending on the buffer's last vertex is kept") {
        rt_instances.items[1].vertex_offset = 3;
        CHECK(kept(hit_on(1, 0, {0.0f, 0.0f})));
    }
}

TEST_CASE("The hit's UV is interpolated, then scaled by the material on each axis it scales", "[rt_hit]") {
    build_scene();

    SECTION("a zero scale leaves its axis alone") {
        const vec2 uv = rt_surface_uv(rt_resolve_hit(hit_on(1, 0, {0.25f, 0.5f})));
        CHECK(uv.x == Approx(0.25f * 2.0f));
        CHECK(uv.y == Approx(0.5f));
    }
    SECTION("so does a negative one") {
        materials.items[1].base_params[8] = -3.0f;
        const vec2 uv = rt_surface_uv(rt_resolve_hit(hit_on(1, 0, {0.25f, 0.5f})));
        CHECK(uv.y == Approx(0.5f));
    }
}

TEST_CASE("The hit's normal is the interpolated vertex normal, turned into the world with its instance", "[rt_hit]") {
    build_scene();

    SECTION("interpolated and normalised") {
        const vec3 n = rt_surface_normal(rt_resolve_hit(hit_on(1, 0, {0.5f, 0.5f})));
        CHECK(n.x == Approx(0.0f).margin(1e-6));
        CHECK(n.y == Approx(0.70710678f));
        CHECK(n.z == Approx(0.70710678f));
    }
    SECTION("rotated and scaled with the instance, never moved") {
        mat4 transform = glm::translate(mat4(1.0f), vec3(10.0f, -4.0f, 2.0f));
        transform = glm::rotate(transform, glm::half_pi<float>(), vec3(0.0f, 1.0f, 0.0f));
        transform = glm::scale(transform, vec3(3.0f));
        // The first vertex alone: its normal is +X, which a quarter turn about
        // +Y carries to -Z.
        const vec3 n = rt_surface_normal(rt_resolve_hit(hit_on(1, 0, {0.0f, 0.0f}, transform)));
        CHECK(n.x == Approx(0.0f).margin(1e-6));
        CHECK(n.y == Approx(0.0f).margin(1e-6));
        CHECK(n.z == Approx(-1.0f));
    }
    SECTION("kept perpendicular to a surface stretched more along one axis than another") {
        // Halfway between the first two vertices the normal is (1, 1, 0) at
        // 45 degrees. Stretching the mesh four times along y lays the surface
        // flatter against it, so its normal turns toward x: (1, 1/4, 0), the
        // inverse transpose, and not (1, 4, 0), the stretch itself.
        const mat4 stretch = glm::scale(mat4(1.0f), vec3(1.0f, 4.0f, 1.0f));
        const vec3 n = rt_surface_normal(rt_resolve_hit(hit_on(1, 0, {0.5f, 0.0f}, stretch)));
        CHECK(n.x == Approx(0.9701425f));
        CHECK(n.y == Approx(0.2425356f));
        CHECK(n.z == Approx(0.0f).margin(1e-6));
    }
}

TEST_CASE("The hit knows whether its instance is lit from either side", "[rt_hit]") {
    build_scene();
    CHECK_FALSE(rt_resolve_hit(hit_on(1, 0, {0.0f, 0.0f})).two_sided);
    rt_instances.items[1].flags = RT_INSTANCE_TWO_SIDED;
    CHECK(rt_resolve_hit(hit_on(1, 0, {0.0f, 0.0f})).two_sided);
}

TEST_CASE("The hit's albedo is the colour factor, times the base colour texture two mips down when it has one", "[rt_hit]") {
    build_scene();
    const vec2 uv{0.3f, 0.7f};

    SECTION("with a texture") {
        const vec3 albedo = rt_surface_albedo(rt_resolve_hit(hit_on(1, 0, {0.0f, 0.0f})), uv);
        REQUIRE(reads.size() == 1);
        CHECK(reads[0].slot == 7);
        CHECK(reads[0].uv == uv);
        CHECK(reads[0].lod == Approx(2.0f));
        CHECK(albedo.x == Approx(0.8f * 0.5f));
        CHECK(albedo.y == Approx(0.6f * 0.25f));
        CHECK(albedo.z == Approx(0.4f * 0.125f));
    }
    SECTION("without one, the factor alone, and nothing is read") {
        materials.items[1].texture_indices[0] = 0;
        const vec3 albedo = rt_surface_albedo(rt_resolve_hit(hit_on(1, 0, {0.0f, 0.0f})), uv);
        CHECK(reads.empty());
        CHECK(albedo == vec3(0.8f, 0.6f, 0.4f));
    }
}

TEST_CASE("The hit's emission reads the emissive slot one mip down, and multiplies it in only when the material has an emissive texture", "[rt_hit]") {
    build_scene();
    const vec2 uv{0.3f, 0.7f};

    SECTION("with an emissive texture") {
        const vec3 emission = rt_surface_emission(rt_resolve_hit(hit_on(1, 0, {0.0f, 0.0f})), uv);
        REQUIRE(reads.size() == 1);
        CHECK(reads[0].slot == 9);
        CHECK(reads[0].lod == Approx(1.0f));
        CHECK(emission.x == Approx(1.0f * 0.5f));
        CHECK(emission.y == Approx(2.0f * 0.25f));
        CHECK(emission.z == Approx(3.0f * 0.125f));
    }
    SECTION("without one, the factor alone") {
        materials.items[1].has_emissive_texture = 0;
        const vec3 emission = rt_surface_emission(rt_resolve_hit(hit_on(1, 0, {0.0f, 0.0f})), uv);
        CHECK(emission == vec3(1.0f, 2.0f, 3.0f));
    }
}
