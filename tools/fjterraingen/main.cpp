// fjterraingen — generates a procedural .fjterrain for testing the terrain system.
//
// Fractal gradient noise, blending rolling lowlands with ridged highlands under
// a slowly varying mask, so the result reads as landforms rather than uniform
// bumps. Deterministic for a given seed.

#include "renderer/resources/fjterrain_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Hash a lattice point to a stable pseudo-random value in [0,1). Integer hash
// rather than a permutation table so the generator carries no state.
float lattice_value(int x, int y, uint32_t seed) {
    uint32_t h = seed;
    h ^= static_cast<uint32_t>(x) * 0x8DA6B343U;
    h ^= static_cast<uint32_t>(y) * 0xD8163841U;
    h ^= h >> 15;
    h *= 0x2C1B3C6DU;
    h ^= h >> 12;
    h *= 0x297A2D39U;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFFFU) / static_cast<float>(0x1000000U);
}

// Smoothstep keeps the lattice interpolation C1-continuous, so shaded normals
// don't crease along cell boundaries.
float smooth(float t) { return t * t * (3.0F - (2.0F * t)); }

// Unit gradient vector for a lattice corner, picked from the hash. Gradient
// noise interpolates these directions rather than raw values, which is what
// keeps features off the grid axes — interpolating values directly produces
// blocky, axis-aligned blobs no amount of octave stacking hides.
void lattice_gradient(int x, int y, uint32_t seed, float& gx, float& gy) {
    constexpr float TWO_PI = 6.283185307F;
    const float angle = lattice_value(x, y, seed) * TWO_PI;
    gx = std::cos(angle);
    gy = std::sin(angle);
}

// Perlin-style gradient noise, remapped to [0,1].
float gradient_noise(float x, float y, uint32_t seed) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int ix = static_cast<int>(fx);
    const int iy = static_cast<int>(fy);
    const float dx = x - fx;
    const float dy = y - fy;
    const float tx = smooth(dx);
    const float ty = smooth(dy);

    auto corner_dot = [&](int cx, int cy, float ox, float oy) {
        float gx = 0.0F;
        float gy = 0.0F;
        lattice_gradient(ix + cx, iy + cy, seed, gx, gy);
        return (gx * (dx - ox)) + (gy * (dy - oy));
    };

    const float d00 = corner_dot(0, 0, 0.0F, 0.0F);
    const float d10 = corner_dot(1, 0, 1.0F, 0.0F);
    const float d01 = corner_dot(0, 1, 0.0F, 1.0F);
    const float d11 = corner_dot(1, 1, 1.0F, 1.0F);

    const float top = d00 + ((d10 - d00) * tx);
    const float bottom = d01 + ((d11 - d01) * tx);
    const float value = top + ((bottom - top) * ty);

    // Gradient noise spans roughly [-0.7, 0.7]; normalize into [0,1].
    return std::clamp((value * 0.7071F) + 0.5F, 0.0F, 1.0F);
}

// Gain below 0.5 keeps the low octaves dominant, so the terrain reads as a few
// large landforms with detail on them rather than as uniform noise.
constexpr float FBM_GAIN = 0.42F;

float fbm(float x, float y, uint32_t seed, int octaves) {
    float sum = 0.0F;
    float amplitude = 1.0F;
    float frequency = 1.0F;
    float total_amplitude = 0.0F;
    for (int o = 0; o < octaves; ++o) {
        sum += gradient_noise(x * frequency, y * frequency, seed + static_cast<uint32_t>(o)) *
               amplitude;
        total_amplitude += amplitude;
        amplitude *= FBM_GAIN;
        frequency *= 2.0F;
    }
    return sum / total_amplitude;
}

// Ridged noise: folding the octave around 0.5 and inverting turns smooth blobs
// into creased ridge lines, which is what makes hills read as landforms.
float ridged(float x, float y, uint32_t seed, int octaves) {
    float sum = 0.0F;
    float amplitude = 1.0F;
    float frequency = 1.0F;
    float total_amplitude = 0.0F;
    for (int o = 0; o < octaves; ++o) {
        const float n =
            gradient_noise(x * frequency, y * frequency, seed + static_cast<uint32_t>(o));
        const float r = 1.0F - std::fabs((n * 2.0F) - 1.0F);
        sum += r * r * amplitude;
        total_amplitude += amplitude;
        amplitude *= FBM_GAIN;
        frequency *= 2.0F;
    }
    return sum / total_amplitude;
}

void print_usage() {
    std::printf(
        "fjterraingen — generate a procedural .fjterrain\n"
        "\n"
        "Usage: fjterraingen <output.fjterrain> [options]\n"
        "\n"
        "  --samples N     height samples per side (default 1024 = 93 patches)\n"
        "                  must be one greater than a multiple of the patch size\n"
        "  --world-size M  terrain extent in metres (default 512)\n"
        "  --height M      world height of a normalized 1.0 sample (default 60)\n"
        "  --offset M      world height of a normalized 0.0 sample (default 0)\n"
        "  --seed N        noise seed (default 1337)\n"
        "  --octaves N     fractal octaves (default 6)\n"
        "  --float32       store heights as float32 instead of unorm16\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0) {
        print_usage();
        return argc < 2 ? 1 : 0;
    }

    const std::string output = argv[1];
    // 1023 quads = 93 patches of 11. A power-of-two sample count would leave a
    // remainder, since patches tile the *quads*, not the samples.
    uint32_t samples = 1024;
    // Not an option: the mesh shaders declare their output at compile time, so
    // the renderer draws this size and no other. A file written with anything
    // else is refused at load.
    const uint32_t patch_quads = fjell::FJTERRAIN_PATCH_QUADS;
    float world_size = 512.0F;
    float height_scale = 60.0F;
    float height_offset = 0.0F;
    uint32_t seed = 1337;
    int octaves = 6;
    bool use_float32 = false;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = (i + 1) < argc;
        if (arg == "--samples" && has_value) {
            samples = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--patch-quads" && has_value) {
            const auto requested = static_cast<uint32_t>(std::atoi(argv[++i]));
            if (requested != patch_quads) {
                std::fprintf(stderr,
                             "--patch-quads %u: the renderer only draws %u, and a file "
                             "written with another value is refused at load\n",
                             requested, patch_quads);
                return 1;
            }
        } else if (arg == "--world-size" && has_value) {
            world_size = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--height" && has_value) {
            height_scale = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--offset" && has_value) {
            height_offset = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--seed" && has_value) {
            seed = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--octaves" && has_value) {
            octaves = std::atoi(argv[++i]);
        } else if (arg == "--float32") {
            use_float32 = true;
        } else {
            std::fprintf(stderr, "unknown or incomplete option: %s\n", arg.c_str());
            return 1;
        }
    }

    if (samples < 2 || ((samples - 1) % patch_quads) != 0) {
        std::fprintf(stderr,
                     "samples (%u) must be one greater than a multiple of the patch "
                     "size (%u): try %u or %u\n",
                     samples, patch_quads,
                     ((samples - 1) / patch_quads) * patch_quads + 1,
                     (((samples - 1) / patch_quads) + 1) * patch_quads + 1);
        return 1;
    }

    fjell::FjTerrainData data;
    data.header.sample_count = samples;
    data.header.patch_quads = patch_quads;
    data.header.world_size = world_size;
    data.header.height_scale = height_scale;
    data.header.height_offset = height_offset;
    data.header.height_format = use_float32 ? fjell::FjTerrainHeightFormat::float32
                                            : fjell::FjTerrainHeightFormat::unorm16;
    data.header.layer_count = 0;
    data.header.weight_count = 0;

    data.heights.resize(static_cast<size_t>(samples) * samples);

    // Three lattice cells across the terrain puts whole landforms at a scale
    // you can see at once; the octaves supply everything finer.
    constexpr float BASE_CELLS = 3.0F;
    // The mask varies more slowly still, so mountainous and gentle areas come
    // in patches rather than alternating every hill.
    constexpr float MASK_CELLS = 1.5F;

    float lowest = 1.0F;
    float highest = 0.0F;

    for (uint32_t z = 0; z < samples; ++z) {
        for (uint32_t x = 0; x < samples; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(samples - 1);
            const float v = static_cast<float>(z) / static_cast<float>(samples - 1);

            // Rolling base terrain, flattened toward the low end so valleys are
            // broad and walkable instead of everything sitting mid-slope.
            const float base = fbm(u * BASE_CELLS, v * BASE_CELLS, seed, octaves);
            const float lowland = base * base;

            // Ridged highlands, offset seed so ridges don't merely trace the
            // base terrain's own shape.
            const float highland = ridged(u * BASE_CELLS, v * BASE_CELLS, seed + 991, octaves);

            // Where the mask is high, highlands dominate. Smoothstep over the
            // middle of its range gives a soft but decisive transition.
            const float raw_mask = fbm(u * MASK_CELLS, v * MASK_CELLS, seed + 4177, 3);
            const float mask = smooth(std::clamp((raw_mask - 0.35F) / 0.3F, 0.0F, 1.0F));

            const float h = std::clamp(((lowland * (1.0F - mask)) + (highland * mask)), 0.0F, 1.0F);

            data.heights[(static_cast<size_t>(z) * samples) + x] = h;
            lowest = std::fmin(lowest, h);
            highest = std::fmax(highest, h);
        }
    }

    auto result = fjell::save_fjterrain(output, data);
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().c_str());
        return 1;
    }

    const uint32_t patches = (samples - 1) / patch_quads;
    std::printf("wrote %s\n", output.c_str());
    std::printf("  %u x %u samples (%u x %u patches of %u quads)\n", samples, samples,
                patches, patches, patch_quads);
    std::printf("  world %.0fm x %.0fm, height %.1fm..%.1fm\n", world_size, world_size,
                height_offset + (lowest * height_scale),
                height_offset + (highest * height_scale));
    std::printf("  format %s\n", use_float32 ? "float32" : "unorm16");
    return 0;
}
