#include "renderer/fjmesh.hpp"
#include "renderer/frustum.hpp"
#include "renderer/meshlet.hpp"
#include "renderer/vertex.hpp"
#include "core/log.hpp"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// Layout validation — must match what the engine runtime expects
static_assert(sizeof(fjell::Vertex) == 60, "Vertex layout changed — .fjmesh format incompatible");
static_assert(sizeof(fjell::Meshlet) == 16, "Meshlet layout changed — .fjmesh format incompatible");
static_assert(sizeof(fjell::MeshletBounds) == 48, "MeshletBounds layout changed — .fjmesh format incompatible");

namespace fs = std::filesystem;

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
    fjell::AABB aabb;
};

static bool import_mesh(const std::string& input_path, ImportResult& result) {
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

    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];

        fjell::FjMeshSubmesh submesh{};
        submesh.vertex_offset = static_cast<uint32_t>(result.vertices.size());
        submesh.first_index = static_cast<uint32_t>(result.indices.size());
        submesh.material_slot = mesh->mMaterialIndex;

        // Extract vertices
        std::vector<fjell::Vertex> mesh_vertices(mesh->mNumVertices);
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            auto& vert = mesh_vertices[v];

            vert.position = {mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z};

            if (mesh->HasVertexColors(0)) {
                vert.color = {mesh->mColors[0][v].r, mesh->mColors[0][v].g, mesh->mColors[0][v].b};
            } else {
                vert.color = glm::vec3(1.0f);
            }

            if (mesh->HasTextureCoords(0)) {
                // Assimp converts glTF UVs to bottom-left origin; convert back to top-left
                // (glTF/Vulkan convention: V=0 at top)
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
                // Handedness: sign of dot(cross(n, t), b)
                float w = (glm::dot(glm::cross(n, t), b) < 0.0f) ? -1.0f : 1.0f;
                vert.tangent = glm::vec4(t, w);
            } else {
                vert.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            }

            result.aabb.expand(vert.position);
        }

        // Extract local indices (0-based within this mesh)
        std::vector<uint32_t> local_indices;
        local_indices.reserve(mesh->mNumFaces * 3);
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            for (unsigned int i = 0; i < face.mNumIndices; ++i) {
                local_indices.push_back(face.mIndices[i]);
            }
        }

        submesh.index_count = static_cast<uint32_t>(local_indices.size());

        // Build global indices (offset into the combined vertex array)
        std::vector<uint32_t> global_indices(local_indices.size());
        for (size_t i = 0; i < local_indices.size(); ++i) {
            global_indices[i] = submesh.vertex_offset + local_indices[i];
        }

        // Collision data (global offsets)
        uint32_t collision_base = static_cast<uint32_t>(result.collision_vertices.size());
        for (const auto& v : mesh_vertices) {
            result.collision_vertices.push_back(v.position);
        }
        for (auto idx : local_indices) {
            result.collision_indices.push_back(collision_base + idx);
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

    return true;
}

static bool write_fjmesh(const std::string& output_path, const ImportResult& result) {
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

    header.collision_vertices_offset = align_to(offset, 4);
    offset = header.collision_vertices_offset + header.collision_vertex_count * sizeof(glm::vec3);

    header.collision_indices_offset = align_to(offset, 4);
    offset = header.collision_indices_offset + header.collision_index_count * sizeof(uint32_t);

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

    std::fclose(f);

    std::printf("wrote %s (%.1f KB)\n", output_path.c_str(),
                static_cast<double>(file_size) / 1024.0);
    std::printf("  %u vertices, %u indices, %u submeshes\n",
                header.vertex_count, header.index_count, header.submesh_count);
    std::printf("  %u meshlets, %u meshlet vertices, %u triangle bytes\n",
                header.meshlet_count, header.meshlet_vertex_count,
                header.meshlet_triangle_byte_count);
    std::printf("  %u collision vertices, %u collision indices\n",
                header.collision_vertex_count, header.collision_index_count);

    return true;
}

static void print_usage(const char* prog) {
    std::fprintf(stderr, "usage: %s <input.glb|gltf|fbx> [-o output.fjmesh]\n", prog);
}

int main(int argc, char* argv[]) {
    fjell::log::init();

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string input_path;
    std::string output_path;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
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
        return 1;
    }

    // Default output: same directory, .fjmesh extension
    if (output_path.empty()) {
        fs::path p(input_path);
        p.replace_extension(".fjmesh");
        output_path = p.string();
    }

    std::printf("importing %s ...\n", input_path.c_str());

    ImportResult result;
    if (!import_mesh(input_path, result)) {
        return 1;
    }

    if (result.vertices.empty()) {
        std::fprintf(stderr, "error: no mesh data found in '%s'\n", input_path.c_str());
        return 1;
    }

    if (!write_fjmesh(output_path, result)) {
        return 1;
    }

    fjell::log::shutdown();
    return 0;
}
