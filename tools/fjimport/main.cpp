#include <cfloat>
#include <chrono>

#include "physics/collision_build.hpp"
#include "renderer/resources/fjmesh.hpp"
#include "renderer/resources/fjmesh_writer.hpp"
#include "renderer/resources/material_asset.hpp"
#include "renderer/frustum.hpp"
#include "renderer/resources/meshlet.hpp"
#include "renderer/resources/sdf_generator.hpp"
#include "renderer/resources/vertex.hpp"
#include "animation/fjskel.hpp"
#include "animation/fjanim.hpp"
#include "core/log.hpp"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <meshoptimizer.h>

#include <nlohmann/json.hpp>

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// ── Import settings (configurable via --settings JSON) ──────────────────

struct ImportSettings {
    bool generate_lods{true};
    uint32_t lod_count{3};
    float lod_thresholds[8]{1.0f, 0.3f, 0.1f};
    float lod_target_ratios[8]{1.0f, 0.5f, 0.25f};
    float lod_target_error{0.02f};
    bool generate_sdf{true};
    uint32_t sdf_resolution{32};
    bool import_animations{true};
    bool import_textures{true};
    std::string texture_dir{"assets/textures"};
    std::string project_root;
};

// Progress callback: (phase, percent 0-100, message)
using ProgressFn = std::function<void(const char* phase, int percent, const char* message)>;

static ImportSettings load_settings(const std::string& path) {
    ImportSettings s;
    std::ifstream f(path);
    if (!f.is_open()) return s;
    auto j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded()) return s;

    if (j.contains("generate_lods")) s.generate_lods = j["generate_lods"].get<bool>();
    if (j.contains("lod_count")) s.lod_count = std::min(j["lod_count"].get<uint32_t>(), 8u);
    if (j.contains("lod_thresholds")) {
        auto& arr = j["lod_thresholds"];
        for (size_t i = 0; i < arr.size() && i < 8; ++i) s.lod_thresholds[i] = arr[i].get<float>();
    }
    if (j.contains("lod_target_ratios")) {
        auto& arr = j["lod_target_ratios"];
        for (size_t i = 0; i < arr.size() && i < 8; ++i) s.lod_target_ratios[i] = arr[i].get<float>();
    }
    if (j.contains("lod_target_error")) s.lod_target_error = j["lod_target_error"].get<float>();
    if (j.contains("generate_sdf")) s.generate_sdf = j["generate_sdf"].get<bool>();
    if (j.contains("sdf_resolution")) s.sdf_resolution = j["sdf_resolution"].get<uint32_t>();
    if (j.contains("import_animations")) s.import_animations = j["import_animations"].get<bool>();
    if (j.contains("import_textures")) s.import_textures = j["import_textures"].get<bool>();
    if (j.contains("texture_dir")) s.texture_dir = j["texture_dir"].get<std::string>();
    if (j.contains("project_root")) s.project_root = j["project_root"].get<std::string>();
    return s;
}

// Default progress: print to stderr (same as original behavior)
static void stderr_progress(const char* /*phase*/, int /*percent*/, const char* message) {
    std::fprintf(stderr, "  %s\n", message);
}

// JSON progress: newline-delimited JSON to stdout
static void json_progress(const char* phase, int percent, const char* message) {
    std::printf("{\"phase\":\"%s\",\"percent\":%d,\"message\":\"%s\"}\n", phase, percent, message);
    std::fflush(stdout);
}

static void json_done(const std::vector<std::string>& output_files) {
    std::string files_json = "[";
    for (size_t i = 0; i < output_files.size(); ++i) {
        if (i > 0) files_json += ",";
        files_json += "\"" + output_files[i] + "\"";
    }
    files_json += "]";
    std::printf("{\"phase\":\"done\",\"percent\":100,\"message\":\"Import complete\",\"output_files\":%s}\n",
                files_json.c_str());
    std::fflush(stdout);
}

static void json_error(const char* message) {
    std::printf("{\"phase\":\"error\",\"percent\":0,\"message\":\"%s\"}\n", message);
    std::fflush(stdout);
}

// Layout validation — must match what the engine runtime expects
static_assert(sizeof(fjell::Vertex) == 60, "Vertex layout changed — .fjmesh format incompatible");
static_assert(sizeof(fjell::SkinnedVertex) == 92, "SkinnedVertex layout changed — .fjmesh format incompatible");
static_assert(sizeof(fjell::Meshlet) == 16, "Meshlet layout changed — .fjmesh format incompatible");
static_assert(sizeof(fjell::MeshletBounds) == 48, "MeshletBounds layout changed — .fjmesh format incompatible");
static_assert(sizeof(fjell::FjMeshLodLevel) == 16, "FjMeshLodLevel layout changed — .fjmesh format incompatible");

namespace fs = std::filesystem;

// Per-mesh data before LOD generation (LOD 0 source)
struct SourceMesh {
    std::vector<fjell::Vertex> vertices;
    std::vector<uint32_t> local_indices;
    uint32_t material_slot;
};

struct ImportResult {
    std::vector<fjell::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<fjell::FjMeshSubmesh> submeshes;
    std::vector<fjell::Meshlet> meshlets;
    std::vector<fjell::MeshletBounds> bounds;
    std::vector<uint32_t> meshlet_vertices;
    std::vector<uint8_t> meshlet_triangles;
    std::vector<glm::vec3> collision_vertices;
    std::vector<uint32_t> collision_indices;

    // Collision geometry as captured from the source mesh. build_collision_geometry
    // derives the arrays above from these, and both are written to the file so a
    // later source change has the original to work from.
    std::vector<glm::vec3> collision_full_vertices;
    std::vector<uint32_t> collision_full_indices;

    std::vector<fjell::FjMeshLodLevel> lod_levels;
    fjell::AABB aabb;

    // SDF data (v4)
    std::vector<int16_t> sdf_data;
    uint32_t sdf_resolution{0};
    glm::vec3 sdf_min{0.0f};
    glm::vec3 sdf_max{0.0f};
};

// Append a submesh to the import result from local vertices/indices, building meshlets
static void append_submesh(ImportResult& result,
                           const std::vector<fjell::Vertex>& mesh_vertices,
                           const std::vector<uint32_t>& local_indices,
                           uint32_t material_slot) {
    if (local_indices.empty()) return;

    fjell::FjMeshSubmesh submesh{};
    submesh.vertex_offset = static_cast<uint32_t>(result.vertices.size());
    submesh.first_index = static_cast<uint32_t>(result.indices.size());
    submesh.material_slot = material_slot;
    submesh.index_count = static_cast<uint32_t>(local_indices.size());

    // Build global indices
    std::vector<uint32_t> global_indices(local_indices.size());
    for (size_t i = 0; i < local_indices.size(); ++i) {
        global_indices[i] = submesh.vertex_offset + local_indices[i];
    }

    // Build meshlets from local indices
    auto meshlet_result = fjell::build_meshlets(
        mesh_vertices.data(), static_cast<uint32_t>(mesh_vertices.size()),
        local_indices.data(), static_cast<uint32_t>(local_indices.size()));

    submesh.first_meshlet = static_cast<uint32_t>(result.meshlets.size());
    submesh.meshlet_count = static_cast<uint32_t>(meshlet_result.meshlets.size());

    // Rebase meshlet offsets to global arrays
    uint32_t vert_idx_base = static_cast<uint32_t>(result.meshlet_vertices.size());
    uint32_t tri_base = static_cast<uint32_t>(result.meshlet_triangles.size()) / 3;

    for (auto& ml : meshlet_result.meshlets) {
        ml.vertex_offset += vert_idx_base;
        ml.triangle_offset += tri_base;
    }

    // Rebase meshlet vertex indices from local to global
    for (auto& v : meshlet_result.meshlet_vertices) {
        v += submesh.vertex_offset;
    }

    // Append to global arrays
    result.vertices.insert(result.vertices.end(), mesh_vertices.begin(), mesh_vertices.end());
    result.indices.insert(result.indices.end(), global_indices.begin(), global_indices.end());
    result.meshlets.insert(result.meshlets.end(),
                           meshlet_result.meshlets.begin(), meshlet_result.meshlets.end());
    result.bounds.insert(result.bounds.end(),
                         meshlet_result.bounds.begin(), meshlet_result.bounds.end());
    result.meshlet_vertices.insert(result.meshlet_vertices.end(),
                                   meshlet_result.meshlet_vertices.begin(),
                                   meshlet_result.meshlet_vertices.end());
    result.meshlet_triangles.insert(result.meshlet_triangles.end(),
                                    meshlet_result.meshlet_triangles.begin(),
                                    meshlet_result.meshlet_triangles.end());
    result.submeshes.push_back(submesh);
}

static bool import_mesh(const std::string& input_path, ImportResult& result,
                        const ImportSettings& settings, const ProgressFn& progress) {
    Assimp::Importer importer;

    constexpr unsigned int flags =
        aiProcess_Triangulate |
        aiProcess_CalcTangentSpace |
        aiProcess_JoinIdenticalVertices;

    const aiScene* scene = importer.ReadFile(input_path, flags);
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        std::fprintf(stderr, "error: Assimp failed to load '%s': %s\n",
                     input_path.c_str(), importer.GetErrorString());
        return false;
    }

    progress("mesh", 10, "Extracting mesh data...");

    // First pass: extract source mesh data
    std::vector<SourceMesh> source_meshes;
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];

        SourceMesh src{};
        src.material_slot = mesh->mMaterialIndex;
        src.vertices.resize(mesh->mNumVertices);

        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            auto& vert = src.vertices[v];

            vert.position = {mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z};

            if (mesh->HasVertexColors(0)) {
                vert.color = {mesh->mColors[0][v].r, mesh->mColors[0][v].g, mesh->mColors[0][v].b};
            } else {
                vert.color = glm::vec3(1.0f);
            }

            if (mesh->HasTextureCoords(0)) {
                vert.tex_coord = {mesh->mTextureCoords[0][v].x, 1.0f - mesh->mTextureCoords[0][v].y};
            } else {
                vert.tex_coord = glm::vec2(0.0f);
            }

            if (mesh->HasNormals()) {
                vert.normal = {mesh->mNormals[v].x, mesh->mNormals[v].y, mesh->mNormals[v].z};
            } else {
                vert.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }

            if (mesh->HasTangentsAndBitangents()) {
                glm::vec3 t = {mesh->mTangents[v].x, mesh->mTangents[v].y, mesh->mTangents[v].z};
                glm::vec3 b = {mesh->mBitangents[v].x, mesh->mBitangents[v].y, mesh->mBitangents[v].z};
                glm::vec3 n = vert.normal;
                float w = (glm::dot(glm::cross(n, t), b) < 0.0f) ? -1.0f : 1.0f;
                vert.tangent = glm::vec4(t, w);
            } else {
                vert.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            }

            result.aabb.expand(vert.position);
        }

        src.local_indices.reserve(mesh->mNumFaces * 3);
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            for (unsigned int i = 0; i < face.mNumIndices; ++i) {
                src.local_indices.push_back(face.mIndices[i]);
            }
        }

        // Collision data (from LOD 0 only)
        uint32_t collision_base = static_cast<uint32_t>(result.collision_vertices.size());
        for (const auto& v : src.vertices) {
            result.collision_vertices.push_back(v.position);
        }
        for (auto idx : src.local_indices) {
            result.collision_indices.push_back(collision_base + idx);
        }

        source_meshes.push_back(std::move(src));
    }

    // Generate LOD levels: LOD 0 uses original indices, LOD 1+ use meshopt_simplify
    uint32_t num_lods = settings.generate_lods ? settings.lod_count : 1;

    for (uint32_t lod = 0; lod < num_lods; ++lod) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Generating LOD %u/%u", lod + 1, num_lods);
        progress("lod", static_cast<int>(25 + 40 * lod / num_lods), msg);

        fjell::FjMeshLodLevel level{};
        level.first_submesh = static_cast<uint32_t>(result.submeshes.size());
        level.max_screen_size = settings.lod_thresholds[lod];

        uint32_t lod_submeshes = 0;

        for (const auto& src : source_meshes) {
            if (src.local_indices.empty()) continue;

            if (lod == 0) {
                // LOD 0: use original geometry
                append_submesh(result, src.vertices, src.local_indices, src.material_slot);
                lod_submeshes++;
            } else {
                // Simplified LOD
                size_t target_count = static_cast<size_t>(
                    static_cast<float>(src.local_indices.size()) * settings.lod_target_ratios[lod]);
                // Round down to multiple of 3
                target_count = (target_count / 3) * 3;
                if (target_count < 3) target_count = 3;

                std::vector<uint32_t> simplified(src.local_indices.size());
                float result_error = 0.0f;
                size_t simplified_count = meshopt_simplify(
                    simplified.data(),
                    src.local_indices.data(),
                    src.local_indices.size(),
                    &src.vertices[0].position.x,
                    src.vertices.size(),
                    sizeof(fjell::Vertex),
                    target_count,
                    settings.lod_target_error,
                    0, // options
                    &result_error);

                simplified.resize(simplified_count);

                // Skip this LOD if simplification didn't reduce enough
                // (keep it anyway as a valid LOD — it just won't save much)
                if (!simplified.empty()) {
                    append_submesh(result, src.vertices, simplified, src.material_slot);
                    lod_submeshes++;
                }
            }
        }

        level.submesh_count = lod_submeshes;
        result.lod_levels.push_back(level);
    }

    // Remove trailing LODs that have 0 submeshes
    while (!result.lod_levels.empty() && result.lod_levels.back().submesh_count == 0) {
        result.lod_levels.pop_back();
    }

    // Generate per-mesh SDF from collision geometry
    if (settings.generate_sdf &&
        !result.collision_vertices.empty() && !result.collision_indices.empty()) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Generating SDF (%ux%ux%u)",
                      settings.sdf_resolution, settings.sdf_resolution, settings.sdf_resolution);
        progress("sdf", 75, msg);

        auto sdf = fjell::generate_sdf(
            result.collision_vertices.data(),
            static_cast<uint32_t>(result.collision_vertices.size()),
            result.collision_indices.data(),
            static_cast<uint32_t>(result.collision_indices.size()),
            result.aabb.min, result.aabb.max, settings.sdf_resolution);
        result.sdf_data = std::move(sdf.data);
        result.sdf_resolution = sdf.resolution;
        result.sdf_min = sdf.sdf_min;
        result.sdf_max = sdf.sdf_max;
    }

    return true;
}

// Replaces the render-geometry copy in `result` with the collision geometry the
// mesh is authored to use. Runs after SDF generation, which wants the full
// detail regardless of what the collider ends up being.
static void build_collision_geometry(ImportResult& result,
                                     const fjell::MeshAuthoredData& authored,
                                     const ProgressFn& progress) {
    using fjell::FjMeshCollisionSource;

    if (result.collision_vertices.empty() || result.collision_indices.empty()) return;

    switch (authored.collision_source) {
        case FjMeshCollisionSource::convex_hull:
            progress("collision", 88, "Building convex hull...");
            break;
        case FjMeshCollisionSource::simplified:
            progress("collision", 88, "Simplifying collision mesh...");
            break;
        default:
            break;
    }

    // Keep what was captured before deriving from it: changing the source later
    // needs the original geometry, and the derived form cannot be inverted.
    result.collision_full_vertices = result.collision_vertices;
    result.collision_full_indices = result.collision_indices;

    fjell::CollisionBuildInput input{&result.collision_vertices, &result.collision_indices};
    auto built = fjell::build_collision(input, authored.collision_source,
                                        authored.collision_simplify_ratio);

    result.collision_vertices = std::move(built.vertices);
    result.collision_indices = std::move(built.indices);
}

static bool write_fjmesh(const std::string& output_path, const ImportResult& result,
                         const fjell::MeshAuthoredData& authored) {
    FILE* f = std::fopen(output_path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "error: cannot open '%s' for writing\n", output_path.c_str());
        return false;
    }

    // Compute section offsets (after header)
    uint64_t offset = sizeof(fjell::FjMeshHeader);

    auto align_to = [](uint64_t val, uint64_t alignment) -> uint64_t {
        return (val + alignment - 1) & ~(alignment - 1);
    };

    fjell::FjMeshHeader header{};
    header.magic = fjell::FJMESH_MAGIC;
    header.version = fjell::FJMESH_VERSION;
    header.flags = 0;

    header.vertex_count = static_cast<uint32_t>(result.vertices.size());
    header.index_count = static_cast<uint32_t>(result.indices.size());
    header.submesh_count = static_cast<uint32_t>(result.submeshes.size());
    header.meshlet_count = static_cast<uint32_t>(result.meshlets.size());
    header.meshlet_vertex_count = static_cast<uint32_t>(result.meshlet_vertices.size());
    header.meshlet_triangle_byte_count = static_cast<uint32_t>(result.meshlet_triangles.size());
    header.collision_vertex_count = static_cast<uint32_t>(result.collision_vertices.size());
    header.collision_index_count = static_cast<uint32_t>(result.collision_indices.size());
    header.max_meshlet_vertices = fjell::MAX_MESHLET_VERTICES;
    header.max_meshlet_triangles = fjell::MAX_MESHLET_TRIANGLES;
    header.lod_count = static_cast<uint32_t>(result.lod_levels.size());
    header.aabb_min = result.aabb.min;
    header.aabb_max = result.aabb.max;

    // Layout sections with 8-byte alignment
    header.vertices_offset = align_to(offset, 8);
    offset = header.vertices_offset + header.vertex_count * sizeof(fjell::Vertex);

    header.indices_offset = align_to(offset, 4);
    offset = header.indices_offset + header.index_count * sizeof(uint32_t);

    header.submeshes_offset = align_to(offset, 4);
    offset = header.submeshes_offset + header.submesh_count * sizeof(fjell::FjMeshSubmesh);

    header.meshlets_offset = align_to(offset, 4);
    offset = header.meshlets_offset + header.meshlet_count * sizeof(fjell::Meshlet);

    header.meshlet_bounds_offset = align_to(offset, 16);
    offset = header.meshlet_bounds_offset + header.meshlet_count * sizeof(fjell::MeshletBounds);

    header.meshlet_vertices_offset = align_to(offset, 4);
    offset = header.meshlet_vertices_offset + header.meshlet_vertex_count * sizeof(uint32_t);

    header.meshlet_triangles_offset = align_to(offset, 4);
    offset = header.meshlet_triangles_offset + header.meshlet_triangle_byte_count;

    // Deriving is a no-op for these sources, so the two collision sections hold
    // the same bytes and the file stores them once.
    const bool collision_is_full_detail =
        authored.collision_source == fjell::FjMeshCollisionSource::render_mesh ||
        authored.collision_source == fjell::FjMeshCollisionSource::custom_mesh;

    header.collision_vertices_offset = align_to(offset, 4);
    offset = header.collision_vertices_offset + header.collision_vertex_count * sizeof(glm::vec3);

    header.collision_indices_offset = align_to(offset, 4);
    offset = header.collision_indices_offset + header.collision_index_count * sizeof(uint32_t);

    // Full-detail collision (v7). A render_mesh source leaves the derived
    // geometry identical to what it came from, so both point at one copy.
    header.collision_full_vertex_count = static_cast<uint32_t>(result.collision_full_vertices.size());
    header.collision_full_index_count = static_cast<uint32_t>(result.collision_full_indices.size());
    if (collision_is_full_detail) {
        header.collision_full_vertices_offset = header.collision_vertices_offset;
        header.collision_full_indices_offset = header.collision_indices_offset;
    } else {
        header.collision_full_vertices_offset = align_to(offset, 4);
        offset = header.collision_full_vertices_offset +
                 header.collision_full_vertex_count * sizeof(glm::vec3);

        header.collision_full_indices_offset = align_to(offset, 4);
        offset = header.collision_full_indices_offset +
                 header.collision_full_index_count * sizeof(uint32_t);
    }

    // Collision shape list (v8). The importer authors none — a freshly
    // imported mesh collides as its geometry until someone says otherwise, and
    // the loader synthesizes that single shape.
    header.collision_primitive_count = 0;
    header.collision_primitives_offset = align_to(offset, 4);

    header.lod_levels_offset = align_to(offset, 4);
    offset = header.lod_levels_offset + header.lod_count * sizeof(fjell::FjMeshLodLevel);

    // SDF section (v4)
    header.sdf_resolution = result.sdf_resolution;
    header.sdf_min = result.sdf_min;
    header.sdf_max = result.sdf_max;
    if (result.sdf_resolution > 0 && !result.sdf_data.empty()) {
        header.sdf_data_offset = align_to(offset, 4);
        offset = header.sdf_data_offset + result.sdf_data.size() * sizeof(int16_t);
    } else {
        header.sdf_data_offset = 0;
    }

    // Authored settings (v5)
    fjell::AuthoredLayout authored_layout;
    offset = fjell::layout_authored_sections(header, offset, authored, authored_layout);

    uint64_t file_size = offset;

    // Write header
    std::fwrite(&header, sizeof(header), 1, f);

    // Helper: pad to target offset
    auto pad_to = [&](uint64_t target) {
        long current = std::ftell(f);
        if (current < 0) return;
        while (static_cast<uint64_t>(current) < target) {
            uint8_t zero = 0;
            std::fwrite(&zero, 1, 1, f);
            ++current;
        }
    };

    // Write sections
    pad_to(header.vertices_offset);
    std::fwrite(result.vertices.data(), sizeof(fjell::Vertex), result.vertices.size(), f);

    pad_to(header.indices_offset);
    std::fwrite(result.indices.data(), sizeof(uint32_t), result.indices.size(), f);

    pad_to(header.submeshes_offset);
    std::fwrite(result.submeshes.data(), sizeof(fjell::FjMeshSubmesh), result.submeshes.size(), f);

    pad_to(header.meshlets_offset);
    std::fwrite(result.meshlets.data(), sizeof(fjell::Meshlet), result.meshlets.size(), f);

    pad_to(header.meshlet_bounds_offset);
    std::fwrite(result.bounds.data(), sizeof(fjell::MeshletBounds), result.bounds.size(), f);

    pad_to(header.meshlet_vertices_offset);
    std::fwrite(result.meshlet_vertices.data(), sizeof(uint32_t), result.meshlet_vertices.size(), f);

    pad_to(header.meshlet_triangles_offset);
    std::fwrite(result.meshlet_triangles.data(), 1, result.meshlet_triangles.size(), f);

    pad_to(header.collision_vertices_offset);
    std::fwrite(result.collision_vertices.data(), sizeof(glm::vec3), result.collision_vertices.size(), f);

    pad_to(header.collision_indices_offset);
    std::fwrite(result.collision_indices.data(), sizeof(uint32_t), result.collision_indices.size(), f);

    if (!collision_is_full_detail) {
        pad_to(header.collision_full_vertices_offset);
        std::fwrite(result.collision_full_vertices.data(), sizeof(glm::vec3),
                    result.collision_full_vertices.size(), f);

        pad_to(header.collision_full_indices_offset);
        std::fwrite(result.collision_full_indices.data(), sizeof(uint32_t),
                    result.collision_full_indices.size(), f);
    }

    pad_to(header.lod_levels_offset);
    std::fwrite(result.lod_levels.data(), sizeof(fjell::FjMeshLodLevel), result.lod_levels.size(), f);

    // SDF data (v4)
    if (header.sdf_data_offset > 0 && !result.sdf_data.empty()) {
        pad_to(header.sdf_data_offset);
        std::fwrite(result.sdf_data.data(), sizeof(int16_t), result.sdf_data.size(), f);
    }

    // Authored settings (v5)
    fjell::write_authored_sections(f, header, authored_layout);

    std::fclose(f);

    std::printf("wrote %s (%.1f KB)\n", output_path.c_str(),
                static_cast<double>(file_size) / 1024.0);
    std::printf("  %u vertices, %u indices, %u submeshes, %u LODs\n",
                header.vertex_count, header.index_count, header.submesh_count,
                header.lod_count);
    std::printf("  %u meshlets, %u meshlet vertices, %u triangle bytes\n",
                header.meshlet_count, header.meshlet_vertex_count,
                header.meshlet_triangle_byte_count);

    // Per-LOD summary
    for (uint32_t i = 0; i < header.lod_count; ++i) {
        const auto& lod = result.lod_levels[i];
        uint32_t lod_meshlets = 0;
        uint32_t lod_tris = 0;
        for (uint32_t s = 0; s < lod.submesh_count; ++s) {
            const auto& sub = result.submeshes[lod.first_submesh + s];
            lod_meshlets += sub.meshlet_count;
            lod_tris += sub.index_count / 3;
        }
        std::printf("  LOD %u: %u submeshes, %u meshlets, %u triangles (threshold %.2f)\n",
                    i, lod.submesh_count, lod_meshlets, lod_tris, lod.max_screen_size);
    }

    return true;
}

// ════════════════════════════════════════════════════════════════════════
// Skinned mesh import
// ════════════════════════════════════════════════════════════════════════

struct SkinnedImportResult {
    std::vector<fjell::SkinnedVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<glm::vec3> collision_vertices;
    std::vector<uint32_t> collision_indices;
    uint32_t joint_count{0};
    fjell::AABB aabb;

    // Joint name → index mapping (built from skeleton hierarchy)
    std::unordered_map<std::string, uint32_t> joint_map;
    // Inverse bind matrices per joint
    std::vector<glm::mat4> inv_bind_matrices;
};

// Walk the aiNode tree and collect joint nodes that match bone names.
// Build the skeleton hierarchy as an ozz RawSkeleton.
static void collect_skeleton_joints(const aiNode* node,
                                    const std::unordered_map<std::string, bool>& bone_names,
                                    ozz::animation::offline::RawSkeleton::Joint& out_joint) {
    out_joint.name = node->mName.C_Str();

    // Decompose aiMatrix4x4 to ozz transform
    aiVector3D pos, scale;
    aiQuaternion rot;
    node->mTransformation.Decompose(scale, rot, pos);
    out_joint.transform.translation = ozz::math::Float3(pos.x, pos.y, pos.z);
    out_joint.transform.rotation = ozz::math::Quaternion(rot.x, rot.y, rot.z, rot.w);
    out_joint.transform.scale = ozz::math::Float3(scale.x, scale.y, scale.z);

    for (unsigned int c = 0; c < node->mNumChildren; ++c) {
        const aiNode* child = node->mChildren[c];
        std::string child_name = child->mName.C_Str();
        // Include this child if it's a bone or if any descendant is a bone
        bool include = bone_names.count(child_name) > 0;
        if (!include) {
            // Check descendants recursively
            std::function<bool(const aiNode*)> has_bone_descendant = [&](const aiNode* n) -> bool {
                for (unsigned int i = 0; i < n->mNumChildren; ++i) {
                    if (bone_names.count(n->mChildren[i]->mName.C_Str()) > 0) return true;
                    if (has_bone_descendant(n->mChildren[i])) return true;
                }
                return false;
            };
            include = has_bone_descendant(child);
        }
        if (include) {
            out_joint.children.emplace_back();
            collect_skeleton_joints(child, bone_names, out_joint.children.back());
        }
    }
}

// Flatten joint names from a built ozz skeleton into a name→index map
static std::unordered_map<std::string, uint32_t>
build_joint_map(const ozz::animation::Skeleton& skeleton) {
    std::unordered_map<std::string, uint32_t> map;
    auto names = skeleton.joint_names();
    for (int i = 0; i < skeleton.num_joints(); ++i) {
        map[names[i]] = static_cast<uint32_t>(i);
    }
    return map;
}

// Find the armature root node (parent of all bones)
static const aiNode* find_armature_root(const aiScene* scene,
                                        const std::unordered_map<std::string, bool>& bone_names) {
    // Find nodes that are bones
    std::function<const aiNode*(const aiNode*)> find = [&](const aiNode* node) -> const aiNode* {
        std::string name = node->mName.C_Str();
        if (bone_names.count(name) > 0) return node;
        for (unsigned int i = 0; i < node->mNumChildren; ++i) {
            auto* found = find(node->mChildren[i]);
            if (found) return found;
        }
        return nullptr;
    };

    // Find the first bone, then walk up to find the armature root
    const aiNode* first_bone = find(scene->mRootNode);
    if (!first_bone) return nullptr;

    // The armature root is the bone node whose parent is not a bone
    const aiNode* armature = first_bone;
    while (armature->mParent && bone_names.count(armature->mParent->mName.C_Str()) > 0) {
        armature = armature->mParent;
    }
    return armature;
}

static bool import_skinned_mesh(const aiScene* scene, SkinnedImportResult& result) {
    // Collect all bone names across all meshes
    std::unordered_map<std::string, bool> bone_names;
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        if (!mesh->HasBones()) continue;
        for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
            bone_names[mesh->mBones[b]->mName.C_Str()] = true;
        }
    }

    if (bone_names.empty()) return false;

    // Find armature root and build ozz RawSkeleton
    const aiNode* armature = find_armature_root(scene, bone_names);
    if (!armature) {
        std::fprintf(stderr, "error: could not find armature root\n");
        return false;
    }

    ozz::animation::offline::RawSkeleton raw_skel;
    raw_skel.roots.resize(1);
    collect_skeleton_joints(armature, bone_names, raw_skel.roots[0]);

    if (!raw_skel.Validate()) {
        std::fprintf(stderr, "error: invalid skeleton hierarchy\n");
        return false;
    }

    // Build runtime skeleton
    ozz::animation::offline::SkeletonBuilder skel_builder;
    auto runtime_skel = skel_builder(raw_skel);
    if (!runtime_skel) {
        std::fprintf(stderr, "error: skeleton build failed\n");
        return false;
    }

    result.joint_count = static_cast<uint32_t>(runtime_skel->num_joints());
    result.joint_map = build_joint_map(*runtime_skel);
    result.inv_bind_matrices.resize(result.joint_count, glm::mat4(1.0f));

    std::printf("  skeleton: %u joints (armature root: %s)\n",
                result.joint_count, armature->mName.C_Str());

    // Collect inverse bind matrices from aiBone data
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        if (!mesh->HasBones()) continue;
        for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
            const aiBone* bone = mesh->mBones[b];
            auto it = result.joint_map.find(bone->mName.C_Str());
            if (it == result.joint_map.end()) continue;
            const aiMatrix4x4& ai_m = bone->mOffsetMatrix;
            result.inv_bind_matrices[it->second] = glm::mat4(
                ai_m.a1, ai_m.b1, ai_m.c1, ai_m.d1,
                ai_m.a2, ai_m.b2, ai_m.c2, ai_m.d2,
                ai_m.a3, ai_m.b3, ai_m.c3, ai_m.d3,
                ai_m.a4, ai_m.b4, ai_m.c4, ai_m.d4);
        }
    }

    // Extract skinned vertices from all meshes
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        if (!mesh->HasBones()) continue;

        uint32_t base_vertex = static_cast<uint32_t>(result.vertices.size());

        // Per-vertex bone influence accumulator
        struct BoneInfluence { uint32_t id; float weight; };
        std::vector<std::vector<BoneInfluence>> per_vertex_bones(mesh->mNumVertices);

        for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
            const aiBone* bone = mesh->mBones[b];
            auto it = result.joint_map.find(bone->mName.C_Str());
            if (it == result.joint_map.end()) continue;
            uint32_t joint_idx = it->second;

            for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
                uint32_t vid = bone->mWeights[w].mVertexId;
                float weight = bone->mWeights[w].mWeight;
                if (weight > 0.0f) {
                    per_vertex_bones[vid].push_back({joint_idx, weight});
                }
            }
        }

        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            fjell::SkinnedVertex vert{};

            vert.position = {mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z};

            if (mesh->HasVertexColors(0)) {
                vert.color = {mesh->mColors[0][v].r, mesh->mColors[0][v].g, mesh->mColors[0][v].b};
            } else {
                vert.color = glm::vec3(1.0f);
            }

            if (mesh->HasTextureCoords(0)) {
                vert.tex_coord = {mesh->mTextureCoords[0][v].x, 1.0f - mesh->mTextureCoords[0][v].y};
            } else {
                vert.tex_coord = glm::vec2(0.0f);
            }

            if (mesh->HasNormals()) {
                vert.normal = {mesh->mNormals[v].x, mesh->mNormals[v].y, mesh->mNormals[v].z};
            } else {
                vert.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }

            if (mesh->HasTangentsAndBitangents()) {
                glm::vec3 t = {mesh->mTangents[v].x, mesh->mTangents[v].y, mesh->mTangents[v].z};
                glm::vec3 b = {mesh->mBitangents[v].x, mesh->mBitangents[v].y, mesh->mBitangents[v].z};
                glm::vec3 n = vert.normal;
                float w = (glm::dot(glm::cross(n, t), b) < 0.0f) ? -1.0f : 1.0f;
                vert.tangent = glm::vec4(t, w);
            } else {
                vert.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            }

            // Bone influences: take top 4, normalize
            auto& influences = per_vertex_bones[v];
            std::sort(influences.begin(), influences.end(),
                      [](const BoneInfluence& a, const BoneInfluence& b) {
                          return a.weight > b.weight;
                      });

            vert.bone_ids = glm::uvec4(0);
            vert.bone_weights = glm::vec4(0.0f);
            float total_weight = 0.0f;
            uint32_t count = std::min(static_cast<uint32_t>(influences.size()), 4u);
            for (uint32_t i = 0; i < count; ++i) {
                vert.bone_ids[i] = influences[i].id;
                vert.bone_weights[i] = influences[i].weight;
                total_weight += influences[i].weight;
            }
            // Renormalize
            if (total_weight > 0.0f) {
                vert.bone_weights /= total_weight;
            } else {
                vert.bone_weights.x = 1.0f; // bind to root if no influences
            }

            result.aabb.expand(vert.position);
            result.vertices.push_back(vert);
        }

        // Indices
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            for (unsigned int i = 0; i < face.mNumIndices; ++i) {
                result.indices.push_back(base_vertex + face.mIndices[i]);
            }
        }

        // Collision data
        uint32_t collision_base = static_cast<uint32_t>(result.collision_vertices.size());
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            result.collision_vertices.push_back(
                {mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z});
        }
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            for (unsigned int i = 0; i < face.mNumIndices; ++i) {
                result.collision_indices.push_back(collision_base + face.mIndices[i]);
            }
        }
    }

    std::printf("  mesh: %zu vertices, %zu indices\n",
                result.vertices.size(), result.indices.size());
    return true;
}

[[maybe_unused]] static bool write_skinned_fjmesh(const std::string& output_path,
                                  const SkinnedImportResult& result) {
    FILE* f = std::fopen(output_path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "error: cannot open '%s' for writing\n", output_path.c_str());
        return false;
    }

    uint64_t offset = sizeof(fjell::FjMeshHeader);
    auto align_to = [](uint64_t val, uint64_t alignment) -> uint64_t {
        return (val + alignment - 1) & ~(alignment - 1);
    };

    fjell::FjMeshHeader header{};
    header.magic = fjell::FJMESH_MAGIC;
    header.version = fjell::FJMESH_VERSION;
    header.flags = fjell::FJMESH_FLAG_SKINNED;
    header.vertex_count = static_cast<uint32_t>(result.vertices.size());
    header.index_count = static_cast<uint32_t>(result.indices.size());
    header.submesh_count = 0;
    header.meshlet_count = 0;
    header.meshlet_vertex_count = 0;
    header.meshlet_triangle_byte_count = 0;
    header.collision_vertex_count = static_cast<uint32_t>(result.collision_vertices.size());
    header.collision_index_count = static_cast<uint32_t>(result.collision_indices.size());
    header.max_meshlet_vertices = fjell::MAX_MESHLET_VERTICES;
    header.max_meshlet_triangles = fjell::MAX_MESHLET_TRIANGLES;
    header.lod_count = 0;
    header.aabb_min = result.aabb.min;
    header.aabb_max = result.aabb.max;
    header.joint_count = result.joint_count;

    // Layout: vertices (SkinnedVertex), indices, collision
    header.vertices_offset = align_to(offset, 8);
    offset = header.vertices_offset + header.vertex_count * sizeof(fjell::SkinnedVertex);

    header.indices_offset = align_to(offset, 4);
    offset = header.indices_offset + header.index_count * sizeof(uint32_t);

    // Empty sections
    header.submeshes_offset = offset;
    header.meshlets_offset = offset;
    header.meshlet_bounds_offset = offset;
    header.meshlet_vertices_offset = offset;
    header.meshlet_triangles_offset = offset;
    header.lod_levels_offset = offset;

    header.collision_vertices_offset = align_to(offset, 4);
    offset = header.collision_vertices_offset + header.collision_vertex_count * sizeof(glm::vec3);

    header.collision_indices_offset = align_to(offset, 4);
    offset = header.collision_indices_offset + header.collision_index_count * sizeof(uint32_t);

    // Skinned meshes always collide against their captured geometry, so the
    // full-detail section is that same geometry rather than a second copy.
    header.collision_full_vertex_count = header.collision_vertex_count;
    header.collision_full_index_count = header.collision_index_count;
    header.collision_full_vertices_offset = header.collision_vertices_offset;
    header.collision_full_indices_offset = header.collision_indices_offset;

    uint64_t file_size = offset;

    std::fwrite(&header, sizeof(header), 1, f);

    auto pad_to = [&](uint64_t target) {
        long current = std::ftell(f);
        if (current < 0) return;
        while (static_cast<uint64_t>(current) < target) {
            uint8_t zero = 0;
            std::fwrite(&zero, 1, 1, f);
            ++current;
        }
    };

    pad_to(header.vertices_offset);
    std::fwrite(result.vertices.data(), sizeof(fjell::SkinnedVertex), result.vertices.size(), f);

    pad_to(header.indices_offset);
    std::fwrite(result.indices.data(), sizeof(uint32_t), result.indices.size(), f);

    pad_to(header.collision_vertices_offset);
    std::fwrite(result.collision_vertices.data(), sizeof(glm::vec3), result.collision_vertices.size(), f);

    pad_to(header.collision_indices_offset);
    std::fwrite(result.collision_indices.data(), sizeof(uint32_t), result.collision_indices.size(), f);

    std::fclose(f);
    std::printf("wrote %s (%.1f KB, skinned, %u joints)\n", output_path.c_str(),
                static_cast<double>(file_size) / 1024.0, header.joint_count);
    return true;
}

static bool write_fjskel(const std::string& output_path,
                          const SkinnedImportResult& mesh_result,
                          const ozz::animation::Skeleton& skeleton) {
    FILE* f = std::fopen(output_path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "error: cannot open '%s' for writing\n", output_path.c_str());
        return false;
    }

    // Extract joint data from ozz skeleton
    int num_joints = skeleton.num_joints();
    auto names = skeleton.joint_names();
    auto parents = skeleton.joint_parents();
    auto rest_poses = skeleton.joint_rest_poses();

    auto lane_f = [](const ozz::math::SimdFloat4& v, int lane) -> float {
        alignas(16) float tmp[4];
        ozz::math::StorePtrU(v, tmp);
        return tmp[lane];
    };

    // Build binary joints + name table
    std::vector<fjell::FjSkelJointBin> bin_joints(num_joints);
    std::vector<char> name_table;
    for (int i = 0; i < num_joints; ++i) {
        int soa_idx = i / 4;
        int lane = i % 4;
        const auto& soa = rest_poses[soa_idx];

        auto& bj = bin_joints[i];
        bj.parent = static_cast<int32_t>(parents[i]);
        bj.translation[0] = lane_f(soa.translation.x, lane);
        bj.translation[1] = lane_f(soa.translation.y, lane);
        bj.translation[2] = lane_f(soa.translation.z, lane);
        bj.rotation[0] = lane_f(soa.rotation.x, lane);
        bj.rotation[1] = lane_f(soa.rotation.y, lane);
        bj.rotation[2] = lane_f(soa.rotation.z, lane);
        bj.rotation[3] = lane_f(soa.rotation.w, lane);
        bj.scale[0] = lane_f(soa.scale.x, lane);
        bj.scale[1] = lane_f(soa.scale.y, lane);
        bj.scale[2] = lane_f(soa.scale.z, lane);

        std::string name(names[i]);
        bj.name_offset = static_cast<uint32_t>(name_table.size());
        bj.name_length = static_cast<uint32_t>(name.size());
        name_table.insert(name_table.end(), name.begin(), name.end());
    }

    auto align_to = [](uint64_t val, uint64_t alignment) -> uint64_t {
        return (val + alignment - 1) & ~(alignment - 1);
    };

    // Compute layout
    uint64_t offset = sizeof(fjell::FjSkelHeader);

    fjell::FjSkelHeader header{};
    header.magic = fjell::FJSKEL_MAGIC;
    header.version = fjell::FJSKEL_VERSION;
    header.vertex_count = static_cast<uint32_t>(mesh_result.vertices.size());
    header.index_count = static_cast<uint32_t>(mesh_result.indices.size());
    header.joint_count = static_cast<uint32_t>(num_joints);
    header.collision_vertex_count = static_cast<uint32_t>(mesh_result.collision_vertices.size());
    header.collision_index_count = static_cast<uint32_t>(mesh_result.collision_indices.size());
    header.aabb_min = mesh_result.aabb.min;
    header.aabb_max = mesh_result.aabb.max;

    header.vertices_offset = align_to(offset, 8);
    offset = header.vertices_offset + header.vertex_count * sizeof(fjell::SkinnedVertex);

    header.indices_offset = align_to(offset, 4);
    offset = header.indices_offset + header.index_count * sizeof(uint32_t);

    header.collision_vertices_offset = align_to(offset, 4);
    offset = header.collision_vertices_offset + header.collision_vertex_count * sizeof(glm::vec3);

    header.collision_indices_offset = align_to(offset, 4);
    offset = header.collision_indices_offset + header.collision_index_count * sizeof(uint32_t);

    header.joints_offset = align_to(offset, 8);
    offset = header.joints_offset + num_joints * sizeof(fjell::FjSkelJointBin);

    header.name_table_offset = align_to(offset, 4);
    header.name_table_size = name_table.size();
    offset = header.name_table_offset + header.name_table_size;

    uint64_t file_size = offset;

    // Write
    auto pad_to = [&](uint64_t target) {
        long current = std::ftell(f);
        if (current < 0) return;
        while (static_cast<uint64_t>(current) < target) {
            uint8_t zero = 0;
            std::fwrite(&zero, 1, 1, f);
            ++current;
        }
    };

    std::fwrite(&header, sizeof(header), 1, f);

    pad_to(header.vertices_offset);
    std::fwrite(mesh_result.vertices.data(), sizeof(fjell::SkinnedVertex), mesh_result.vertices.size(), f);

    pad_to(header.indices_offset);
    std::fwrite(mesh_result.indices.data(), sizeof(uint32_t), mesh_result.indices.size(), f);

    pad_to(header.collision_vertices_offset);
    std::fwrite(mesh_result.collision_vertices.data(), sizeof(glm::vec3), mesh_result.collision_vertices.size(), f);

    pad_to(header.collision_indices_offset);
    std::fwrite(mesh_result.collision_indices.data(), sizeof(uint32_t), mesh_result.collision_indices.size(), f);

    pad_to(header.joints_offset);
    std::fwrite(bin_joints.data(), sizeof(fjell::FjSkelJointBin), bin_joints.size(), f);

    pad_to(header.name_table_offset);
    std::fwrite(name_table.data(), 1, name_table.size(), f);

    std::fclose(f);
    std::printf("wrote %s (%.1f KB, %u verts, %u indices, %d joints)\n",
                output_path.c_str(), static_cast<double>(file_size) / 1024.0,
                header.vertex_count, header.index_count, num_joints);
    return true;
}

[[maybe_unused]] static bool export_skeleton(const std::string& output_path,
                             const ozz::animation::Skeleton& skeleton) {
    // Flatten ozz skeleton into JSON joint array (depth-first order)
    int num_joints = skeleton.num_joints();
    auto names = skeleton.joint_names();
    auto parents = skeleton.joint_parents();
    auto rest_poses = skeleton.joint_rest_poses();

    // Helper: extract a float from lane `lane` of a SimdFloat4
    auto lane_f = [](const ozz::math::SimdFloat4& v, int lane) -> float {
        alignas(16) float tmp[4];
        ozz::math::StorePtrU(v, tmp);
        return tmp[lane];
    };

    nlohmann::json j;
    j["joints"] = nlohmann::json::array();

    for (int i = 0; i < num_joints; ++i) {
        int soa_idx = i / 4;
        int lane = i % 4;
        const auto& soa = rest_poses[soa_idx];

        nlohmann::json jj;
        jj["name"] = std::string(names[i]);
        jj["parent"] = static_cast<int>(parents[i]);
        jj["translation"] = {
            lane_f(soa.translation.x, lane),
            lane_f(soa.translation.y, lane),
            lane_f(soa.translation.z, lane)
        };
        jj["rotation"] = {
            lane_f(soa.rotation.x, lane),
            lane_f(soa.rotation.y, lane),
            lane_f(soa.rotation.z, lane),
            lane_f(soa.rotation.w, lane)
        };
        jj["scale"] = {
            lane_f(soa.scale.x, lane),
            lane_f(soa.scale.y, lane),
            lane_f(soa.scale.z, lane)
        };
        j["joints"].push_back(jj);
    }

    j["slots"] = nlohmann::json::array();

    std::ofstream file(output_path);
    if (!file.is_open()) {
        std::fprintf(stderr, "error: cannot open '%s' for writing\n", output_path.c_str());
        return false;
    }
    file << j.dump(2);
    std::printf("wrote %s (%d joints)\n", output_path.c_str(), num_joints);
    return true;
}

static bool export_animation(const std::string& output_path,
                              const aiAnimation* anim,
                              const ozz::animation::Skeleton& skeleton,
                              const std::unordered_map<std::string, uint32_t>& joint_map) {
    int num_joints = skeleton.num_joints();

    float tick_rate = static_cast<float>(anim->mTicksPerSecond);
    if (tick_rate <= 0.0f) tick_rate = 24.0f;

    // Build per-joint track data
    nlohmann::json j;
    j["name"] = std::string(anim->mName.C_Str());

    // Initialize empty tracks for all joints
    j["tracks"] = nlohmann::json::array();
    for (int i = 0; i < num_joints; ++i) {
        j["tracks"].push_back({
            {"translations", nlohmann::json::array()},
            {"rotations", nlohmann::json::array()},
            {"scales", nlohmann::json::array()}
        });
    }

    for (unsigned int c = 0; c < anim->mNumChannels; ++c) {
        const aiNodeAnim* channel = anim->mChannels[c];
        auto it = joint_map.find(channel->mNodeName.C_Str());
        if (it == joint_map.end()) continue;
        uint32_t joint_idx = it->second;

        auto& jtrack = j["tracks"][joint_idx];

        for (unsigned int k = 0; k < channel->mNumPositionKeys; ++k) {
            const auto& key = channel->mPositionKeys[k];
            float time = static_cast<float>(key.mTime) / tick_rate;
            jtrack["translations"].push_back({time,
                {key.mValue.x, key.mValue.y, key.mValue.z}});
        }

        for (unsigned int k = 0; k < channel->mNumRotationKeys; ++k) {
            const auto& key = channel->mRotationKeys[k];
            float time = static_cast<float>(key.mTime) / tick_rate;
            jtrack["rotations"].push_back({time,
                {key.mValue.x, key.mValue.y, key.mValue.z, key.mValue.w}});
        }

        for (unsigned int k = 0; k < channel->mNumScalingKeys; ++k) {
            const auto& key = channel->mScalingKeys[k];
            float time = static_cast<float>(key.mTime) / tick_rate;
            jtrack["scales"].push_back({time,
                {key.mValue.x, key.mValue.y, key.mValue.z}});
        }
    }

    j["events"] = nlohmann::json::array();

    std::ofstream file(output_path);
    if (!file.is_open()) {
        std::fprintf(stderr, "error: cannot open '%s' for writing\n", output_path.c_str());
        return false;
    }
    file << j.dump();  // compact output — animation files can be large
    return true;
}

// ════════════════════════════════════════════════════════════════════════

// ── Material + texture export ───────────────────────────────────────────
//
// Assimp reports textures either embedded (path "*N", indexing
// scene->mTextures) or external (a path relative to the source file). Both
// are written into <output_dir>/<texture_subdir>/ so the resulting .fjmat
// references files that live alongside the .fjmesh.

namespace {

struct TextureSlot {
    aiTextureType type;
    const char* fjmat_name; // slot name the standard PBR shader resolves
};

// Slot names must match the ones ResourceManager resolves for .fjmat files.
constexpr TextureSlot TEXTURE_SLOTS[] = {
    {aiTextureType_BASE_COLOR, "base_color"},
    {aiTextureType_DIFFUSE, "base_color"},
    {aiTextureType_NORMALS, "normal_map"},
    {aiTextureType_DIFFUSE_ROUGHNESS, "metallic_roughness"},
    {aiTextureType_METALNESS, "metallic_roughness"},
    {aiTextureType_EMISSIVE, "emissive"},
};

std::string sanitize_filename(std::string name) {
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == ' ' || c == '*' || c == '?') c = '_';
    }
    return name;
}

// Extension for an embedded texture, from Assimp's format hint ("jpg", "png").
std::string embedded_extension(const aiTexture* tex) {
    std::string hint = tex->achFormatHint;
    if (hint.empty()) return ".bin";
    if (hint == "jpeg") hint = "jpg";
    return "." + hint;
}

/// Write one texture to disk, returning the path to record in the .fjmat.
/// Returns empty on failure. Results are memoized by source reference so a
/// texture reused across slots or materials is written once.
std::string export_texture(const aiScene* scene,
                           const std::string& ref,
                           const std::string& slot_name,
                           const std::filesystem::path& source_dir,
                           const std::filesystem::path& tex_dir,
                           const std::string& tex_ref_prefix,
                           std::unordered_map<std::string, std::string>& cache) {
    namespace fs = std::filesystem;

    auto cached = cache.find(ref);
    if (cached != cache.end()) return cached->second;

    std::string relative;

    if (!ref.empty() && ref[0] == '*') {
        // Embedded: "*N" indexes scene->mTextures.
        int index = std::atoi(ref.c_str() + 1);
        if (index < 0 || static_cast<unsigned>(index) >= scene->mNumTextures) {
            std::fprintf(stderr, "warning: embedded texture index %d out of range\n", index);
            cache[ref] = {};
            return {};
        }
        const aiTexture* tex = scene->mTextures[index];

        // mHeight == 0 means the payload is a compressed file image of
        // mWidth bytes. Raw (uncompressed) embedded textures would need an
        // encoder, so they are skipped rather than written as unreadable data.
        if (tex->mHeight != 0) {
            std::fprintf(stderr, "warning: skipping raw embedded texture %d (no encoder)\n", index);
            cache[ref] = {};
            return {};
        }

        fs::path out_name = sanitize_filename(slot_name) + embedded_extension(tex);

        fs::create_directories(tex_dir);
        fs::path out_path = tex_dir / out_name;
        std::ofstream f(out_path, std::ios::binary);
        if (!f.is_open()) {
            std::fprintf(stderr, "warning: could not write texture '%s'\n", out_path.string().c_str());
            cache[ref] = {};
            return {};
        }
        f.write(reinterpret_cast<const char*>(tex->pcData), tex->mWidth);
        relative = tex_ref_prefix + out_name.string();
    } else {
        // External: resolve against the source file's directory.
        fs::path src = fs::path(ref);
        if (src.is_relative()) src = source_dir / src;

        std::error_code ec;
        if (!fs::exists(src, ec)) {
            std::fprintf(stderr, "warning: texture not found: %s\n", src.string().c_str());
            cache[ref] = {};
            return {};
        }

        fs::create_directories(tex_dir);
        fs::path out_name = sanitize_filename(slot_name) + src.extension().string();
        fs::path out_path = tex_dir / out_name;
        fs::copy_file(src, out_path, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::fprintf(stderr, "warning: could not copy texture '%s': %s\n",
                         src.string().c_str(), ec.message().c_str());
            cache[ref] = {};
            return {};
        }
        relative = tex_ref_prefix + out_name.string();
    }

    cache[ref] = relative;
    return relative;
}

} // namespace

/// Export textures and a .fjmat per referenced material slot. Appends every
/// file written to output_files.
static void export_materials(const aiScene* scene,
                             const std::string& input_path,
                             const std::string& output_path,
                             const ImportSettings& settings,
                             std::vector<std::string>& output_files,
                             std::unordered_map<unsigned int, std::string>& slot_assets) {
    namespace fs = std::filesystem;

    // Only materials some mesh actually uses — importers synthesize an unused
    // default slot that would otherwise produce a junk .fjmat.
    std::vector<bool> referenced(scene->mNumMaterials, false);
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        unsigned int slot = scene->mMeshes[m]->mMaterialIndex;
        if (slot < referenced.size()) referenced[slot] = true;
    }

    fs::path out_dir = fs::path(output_path).parent_path();
    std::string stem = fs::path(output_path).stem().string();
    fs::path source_dir = fs::path(input_path).parent_path();

    // The runtime resolves .fjmat texture paths against the project root, so
    // textures go in a project-relative directory and are referenced that way.
    // Without a known root, fall back to a folder beside the mesh.
    bool have_root = !settings.project_root.empty();
    fs::path tex_dir;
    std::string tex_ref_prefix;
    if (have_root) {
        fs::path relative_dir = fs::path(settings.texture_dir) / stem;
        tex_dir = fs::path(settings.project_root) / relative_dir;
        tex_ref_prefix = relative_dir.generic_string() + "/";
    } else {
        tex_dir = out_dir / "textures";
        tex_ref_prefix = "textures/";
    }

    std::unordered_map<std::string, std::string> texture_cache;
    uint32_t material_count = 0;

    for (unsigned int m = 0; m < scene->mNumMaterials; ++m) {
        if (!referenced[m]) continue;

        const aiMaterial* mat = scene->mMaterials[m];
        fjell::MaterialAsset asset;
        asset.shader = "shaders/standard.fjsl";

        for (const auto& slot : TEXTURE_SLOTS) {
            if (asset.textures.contains(slot.fjmat_name)) continue; // first match wins
            if (mat->GetTextureCount(slot.type) == 0) continue;

            aiString tex_path;
            if (mat->GetTexture(slot.type, 0, &tex_path) != AI_SUCCESS) continue;

            std::string reference = export_texture(scene, tex_path.C_Str(), slot.fjmat_name,
                                                   source_dir, tex_dir, tex_ref_prefix,
                                                   texture_cache);
            if (reference.empty()) continue;

            asset.textures[slot.fjmat_name] = reference;

            std::string written = have_root
                ? (fs::path(settings.project_root) / reference).string()
                : (out_dir / reference).string();
            if (std::find(output_files.begin(), output_files.end(), written) == output_files.end()) {
                output_files.push_back(written);
            }
        }

        if (asset.textures.empty()) continue; // nothing worth writing

        // Name after the mesh so assets stay greppable — source material names
        // are often generator UUIDs. Suffix only when there is more than one.
        std::string name = stem;
        if (material_count > 0) name += "_" + std::to_string(material_count);
        ++material_count;

        fs::path mat_path = out_dir / (name + ".fjmat");
        try {
            fjell::save_material_asset(mat_path.string(), asset);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "warning: could not write %s: %s\n",
                         mat_path.string().c_str(), e.what());
            continue;
        }
        output_files.push_back(mat_path.string());

        // The mesh records this against the source material name, so the
        // assignment is what the runtime binds and what a reimport preserves.
        slot_assets[m] = have_root
            ? fs::relative(mat_path, fs::path(settings.project_root)).generic_string()
            : mat_path.filename().generic_string();
    }
}

// Builds the authored block for a freshly imported mesh: one entry per
// referenced material slot, keyed by the source material name so a later
// reimport can match assignments even if the exporter reorders slots.
static fjell::MeshAuthoredData build_authored_data(
        const aiScene* scene, const std::string& input_path,
        const std::string& output_path,
        const std::unordered_map<unsigned int, std::string>& slot_assets) {
    namespace fs = std::filesystem;
    fjell::MeshAuthoredData authored;

    // Store the path relative to the .fjmesh so a reimport finds the source
    // wherever the pair is moved to together. A source on an unrelated tree
    // produces a long climb out of the project, which would break as soon as
    // either side moved — record just the filename in that case and let the
    // lookup fall back to a search beside the mesh.
    std::error_code rel_ec;
    auto rel = fs::relative(fs::absolute(input_path),
                            fs::absolute(fs::path(output_path).parent_path()), rel_ec);
    bool escapes_tree = rel.empty() ||
        rel.generic_string().starts_with("../../..");
    authored.source_file = (!rel_ec && !escapes_tree)
        ? rel.generic_string()
        : fs::path(input_path).filename().generic_string();

    // file_time_type's epoch is unspecified, so convert to the system clock
    // before recording seconds — the value is compared against source files
    // by a later reimport and needs a stable meaning.
    std::error_code ec;
    auto write_time = fs::last_write_time(input_path, ec);
    if (!ec) {
        auto sys_time = std::chrono::clock_cast<std::chrono::system_clock>(write_time);
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
            sys_time.time_since_epoch()).count();
        if (seconds > 0) {
            authored.source_modified_time = static_cast<uint64_t>(seconds);
        }
    }

    std::vector<bool> referenced(scene->mNumMaterials, false);
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        unsigned int slot = scene->mMeshes[m]->mMaterialIndex;
        if (slot < referenced.size()) referenced[slot] = true;
    }

    // Slot index is the position in this array, matching FjMeshSubmesh's
    // material_slot, so unreferenced slots still occupy their index.
    for (unsigned int m = 0; m < scene->mNumMaterials; ++m) {
        fjell::MeshMaterialSlot slot;
        aiString name;
        if (scene->mMaterials[m]->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) {
            slot.name = name.C_Str();
        }
        if (slot.name.empty()) slot.name = "material_" + std::to_string(m);

        if (auto it = slot_assets.find(m); it != slot_assets.end()) {
            slot.path = it->second;
        }
        authored.material_slots.push_back(std::move(slot));
    }

    // Trim trailing slots nothing references — importers synthesize a default.
    while (!authored.material_slots.empty() &&
           !referenced[authored.material_slots.size() - 1]) {
        authored.material_slots.pop_back();
    }

    return authored;
}

static void print_usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s <input.glb|gltf|fbx> [-o output.fjmesh] [--settings path.json] [--json-progress]\n",
        prog);
}

int main(int argc, char* argv[]) {
    fjell::log::init();

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string input_path;
    std::string output_path;
    std::string settings_path;
    bool use_json_progress = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (std::strcmp(argv[i], "--settings") == 0 && i + 1 < argc) {
            settings_path = argv[++i];
        } else if (std::strcmp(argv[i], "--json-progress") == 0) {
            use_json_progress = true;
        } else if (argv[i][0] != '-') {
            input_path = argv[i];
        } else {
            std::fprintf(stderr, "unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (input_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    if (!fs::exists(input_path)) {
        std::fprintf(stderr, "error: file not found: %s\n", input_path.c_str());
        if (use_json_progress) json_error("File not found");
        return 1;
    }

    // Load settings (defaults if no --settings provided)
    ImportSettings settings;
    if (!settings_path.empty()) {
        settings = load_settings(settings_path);
    }

    ProgressFn progress = use_json_progress ? json_progress : stderr_progress;

    // Default output: same directory, .fjmesh extension
    if (output_path.empty()) {
        fs::path p(input_path);
        p.replace_extension(".fjmesh");
        output_path = p.string();
    }

    if (!use_json_progress) {
        std::printf("importing %s ...\n", input_path.c_str());
    }

    progress("loading", 0, "Loading scene with Assimp...");

    // Load via Assimp first to detect skinned vs static
    Assimp::Importer importer;
    constexpr unsigned int assimp_flags =
        aiProcess_Triangulate |
        aiProcess_CalcTangentSpace |
        aiProcess_JoinIdenticalVertices;
    const aiScene* scene = importer.ReadFile(input_path, assimp_flags);
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        std::fprintf(stderr, "error: Assimp failed to load '%s': %s\n",
                     input_path.c_str(), importer.GetErrorString());
        if (use_json_progress) json_error("Assimp failed to load file");
        return 1;
    }

    // Check if any mesh has bones
    bool has_skin = false;
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        if (scene->mMeshes[m]->HasBones()) { has_skin = true; break; }
    }

    // Track output files for JSON progress
    std::vector<std::string> output_files;

    // Materials are exported before either mesh path writes, so the static
    // path can record the resulting .fjmat paths against its material slots.
    std::unordered_map<unsigned int, std::string> slot_assets;
    if (settings.import_textures && scene->mNumMaterials > 0) {
        progress("textures", 85, "Exporting textures and materials...");
        export_materials(scene, input_path, output_path, settings,
                         output_files, slot_assets);
    }

    if (has_skin) {
        // ── Skinned mesh path ──
        progress("mesh", 10, "Extracting skinned mesh data...");

        SkinnedImportResult skinned_result;
        if (!import_skinned_mesh(scene, skinned_result)) {
            std::fprintf(stderr, "error: skinned import failed\n");
            if (use_json_progress) json_error("Skinned import failed");
            return 1;
        }

        if (skinned_result.vertices.empty()) {
            std::fprintf(stderr, "error: no skinned mesh data found\n");
            if (use_json_progress) json_error("No skinned mesh data found");
            return 1;
        }

        progress("skeleton", 40, "Building skeleton...");

        // Build runtime skeleton
        std::unordered_map<std::string, bool> bone_names;
        for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
            const aiMesh* mesh = scene->mMeshes[m];
            if (!mesh->HasBones()) continue;
            for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
                bone_names[mesh->mBones[b]->mName.C_Str()] = true;
            }
        }
        const aiNode* armature = find_armature_root(scene, bone_names);
        ozz::animation::offline::RawSkeleton raw_skel;
        raw_skel.roots.resize(1);
        collect_skeleton_joints(armature, bone_names, raw_skel.roots[0]);
        ozz::animation::offline::SkeletonBuilder skel_builder;
        auto runtime_skel = skel_builder(raw_skel);

        // Write .fjskel (mesh + skeleton combined)
        progress("write", 60, "Writing .fjskel...");
        fs::path skel_path = fs::path(output_path).replace_extension(".fjskel");
        if (!write_fjskel(skel_path.string(), skinned_result, *runtime_skel)) {
            if (use_json_progress) json_error("Failed to write .fjskel");
            return 1;
        }
        output_files.push_back(skel_path.string());

        // Export animations
        if (settings.import_animations && scene->mNumAnimations > 0) {
            progress("animation", 70, "Exporting animations...");
            fs::path stem = fs::path(output_path).stem();
            fs::path out_dir = fs::path(output_path).parent_path();
            uint32_t exported = 0;
            for (unsigned int a = 0; a < scene->mNumAnimations; ++a) {
                const aiAnimation* anim = scene->mAnimations[a];
                std::string clip_name = anim->mName.C_Str();
                // Sanitize clip name for filename
                for (char& c : clip_name) {
                    if (c == '/' || c == '\\' || c == ':' || c == ' ') c = '_';
                }
                fs::path anim_path = out_dir / (stem.string() + "_" + clip_name + ".fjanim");
                if (export_animation(anim_path.string(), anim, *runtime_skel,
                                     skinned_result.joint_map)) {
                    output_files.push_back(anim_path.string());
                    exported++;
                }
            }
            if (!use_json_progress) {
                std::printf("exported %u/%u animation clips\n", exported, scene->mNumAnimations);
            }
        }
    } else {
        // ── Static mesh path ──
        ImportResult result;
        if (!import_mesh(input_path, result, settings, progress)) {
            if (use_json_progress) json_error("Mesh import failed");
            return 1;
        }

        if (result.vertices.empty()) {
            std::fprintf(stderr, "error: no mesh data found in '%s'\n", input_path.c_str());
            if (use_json_progress) json_error("No mesh data found");
            return 1;
        }

        auto authored = build_authored_data(scene, input_path, output_path, slot_assets);
        merge_authored_data(authored, fjell::read_authored_data(output_path));
        build_collision_geometry(result, authored, progress);

        progress("write", 90, "Writing .fjmesh...");
        if (!write_fjmesh(output_path, result, authored)) {
            if (use_json_progress) json_error("Failed to write .fjmesh");
            return 1;
        }
        output_files.push_back(output_path);
    }

    if (use_json_progress) {
        json_done(output_files);
    }

    fjell::log::shutdown();
    return 0;
}
