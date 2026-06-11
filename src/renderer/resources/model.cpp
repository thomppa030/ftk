#include "renderer/resources/model.hpp"
#include "renderer/resources/fjmesh.hpp"
#include "renderer/resources/meshlet.hpp"
#include "renderer/resources/sdf_generator.hpp"
#include "renderer/resources/meshlet_buffer.hpp"
#include "renderer/resources/primitives.hpp"
#include "renderer/resources/vertex.hpp"
#include "core/log.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/glm_element_traits.hpp>

#include <stb_image.h>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>

static_assert(sizeof(fjell::Vertex) == 60, "Vertex layout changed — .fjmesh format incompatible");
static_assert(sizeof(fjell::Meshlet) == 16, "Meshlet layout changed — .fjmesh format incompatible");
static_assert(sizeof(fjell::MeshletBounds) == 48, "MeshletBounds layout changed — .fjmesh format incompatible");

namespace fjell {

struct LoadedImage {
    std::vector<uint8_t> pixels;
    uint32_t width;
    uint32_t height;
};

static LoadedImage load_image_from_source(
    const fastgltf::Asset& asset,
    const fastgltf::Image& image,
    const std::filesystem::path& base_dir) {
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = nullptr;

    std::visit(fastgltf::visitor{
        [](auto&) {},
        [&](const fastgltf::sources::URI& uri) {
            auto path = base_dir / uri.uri.fspath();
            pixels = stbi_load(path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
        },
        [&](const fastgltf::sources::Array& array) {
            pixels = stbi_load_from_memory(
                reinterpret_cast<const stbi_uc*>(array.bytes.data()),
                static_cast<int>(array.bytes.size()),
                &width, &height, &channels, STBI_rgb_alpha);
        },
        [&](const fastgltf::sources::BufferView& bv) {
            auto& view = asset.bufferViews[bv.bufferViewIndex];
            auto& buffer = asset.buffers[view.bufferIndex];
            if (auto* array = std::get_if<fastgltf::sources::Array>(&buffer.data)) {
                pixels = stbi_load_from_memory(
                    reinterpret_cast<const stbi_uc*>(array->bytes.data() + view.byteOffset),
                    static_cast<int>(view.byteLength),
                    &width, &height, &channels, STBI_rgb_alpha);
            }
        },
    }, image.data);

    if (!pixels) {
        throw std::runtime_error("Failed to load glTF image: " + std::string(image.name));
    }

    size_t size = static_cast<size_t>(width) * height * 4;
    LoadedImage result{
        {pixels, pixels + size},
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height)
    };
    stbi_image_free(pixels);
    return result;
}

Model::Model(VmaAllocator allocator, VkDevice device, VkPhysicalDevice physical_device,
             UploadContext& upload,
             const std::string& path, MegaBuffer* mega,
             MeshletBuffer* meshlet_buf) {
    auto gltf_path = std::filesystem::path(path);
    auto base_dir = gltf_path.parent_path();

    fastgltf::Parser parser;
    auto data = fastgltf::GltfDataBuffer::FromPath(gltf_path);
    if (data.error() != fastgltf::Error::None) {
        throw std::runtime_error("Failed to read glTF file: " + path);
    }

    auto options = fastgltf::Options::LoadExternalBuffers |
                   fastgltf::Options::LoadExternalImages |
                   fastgltf::Options::GenerateMeshIndices;

    auto result = parser.loadGltf(data.get(), base_dir, options);

    if (result.error() != fastgltf::Error::None) {
        throw std::runtime_error("Failed to parse glTF: " + path +
            " (error: " + std::to_string(static_cast<int>(result.error())) + ")");
    }

    auto& asset = result.get();

    FJELL_GFX_DEBUG("Loading model: {} ({} meshes, {} textures)",
                    path, asset.meshes.size(), asset.images.size());

    // Load textures from embedded/external images
    for (auto& image : asset.images) {
        auto [pixels, tex_w, tex_h] = load_image_from_source(asset, image, base_dir);
        textures_.push_back(std::make_unique<Texture>(
            device, allocator, physical_device, upload,
            pixels.data(), tex_w, tex_h));
    }

    // Load meshes
    for (auto& mesh : asset.meshes) {
        for (auto& primitive : mesh.primitives) {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;

            // Indices
            if (primitive.indicesAccessor.has_value()) {
                auto& accessor = asset.accessors[*primitive.indicesAccessor];
                indices.reserve(accessor.count);
                fastgltf::iterateAccessor<uint32_t>(asset, accessor,
                    [&](uint32_t idx) { indices.push_back(idx); });
            }

            // Positions
            auto* pos_it = primitive.findAttribute("POSITION");
            if (pos_it == primitive.attributes.end()) continue;
            auto& pos_accessor = asset.accessors[pos_it->accessorIndex];
            vertices.resize(pos_accessor.count);

            fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, pos_accessor,
                [&](glm::vec3 pos, size_t idx) {
                    vertices[idx].position = pos;
                    vertices[idx].color = {1.0f, 1.0f, 1.0f};
                    vertices[idx].tex_coord = {0.0f, 0.0f};
                });

            // Normals
            auto* norm_it = primitive.findAttribute("NORMAL");
            if (norm_it != primitive.attributes.end()) {
                auto& norm_accessor = asset.accessors[norm_it->accessorIndex];
                fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, norm_accessor,
                    [&](glm::vec3 n, size_t idx) {
                        vertices[idx].normal = n;
                    });
            }

            // Tangents
            auto* tan_it = primitive.findAttribute("TANGENT");
            if (tan_it != primitive.attributes.end()) {
                auto& tan_accessor = asset.accessors[tan_it->accessorIndex];
                fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, tan_accessor,
                    [&](glm::vec4 t, size_t idx) {
                        vertices[idx].tangent = t;
                    });
            }

            // Tex coords
            auto* uv_it = primitive.findAttribute("TEXCOORD_0");
            if (uv_it != primitive.attributes.end()) {
                auto& uv_accessor = asset.accessors[uv_it->accessorIndex];
                fastgltf::iterateAccessorWithIndex<glm::vec2>(asset, uv_accessor,
                    [&](glm::vec2 uv, size_t idx) {
                        vertices[idx].tex_coord = uv;
                    });
            }

            // Vertex colors
            auto* col_it = primitive.findAttribute("COLOR_0");
            if (col_it != primitive.attributes.end()) {
                auto& col_accessor = asset.accessors[col_it->accessorIndex];
                fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, col_accessor,
                    [&](glm::vec3 col, size_t idx) {
                        vertices[idx].color = col;
                    });
            }

            if (!vertices.empty() && !indices.empty()) {
                // Retain CPU vertex positions for physics mesh colliders
                uint32_t base_index = static_cast<uint32_t>(collision_vertices_.size());
                for (const auto& v : vertices) {
                    collision_vertices_.push_back(v.position);
                    local_aabb_.expand(v.position);
                }
                for (auto idx : indices) {
                    collision_indices_.push_back(base_index + idx);
                }

                // Build meshlets for this sub-mesh
                auto meshlet_result = build_meshlets(
                    vertices.data(), static_cast<uint32_t>(vertices.size()),
                    indices.data(), static_cast<uint32_t>(indices.size()));
                meshlet_results_.push_back(meshlet_result);

                if (mega) {
                    auto entry = mega->upload(vertices, indices);
                    if (meshlet_buf && !meshlet_result.meshlets.empty()) {
                        auto range = meshlet_buf->upload(meshlet_result, entry.vertex_offset);
                        entry.meshlet_offset = range.meshlet_offset;
                        entry.meshlet_count = range.meshlet_count;
                    }
                    mega_entries_.push_back(entry);
                    meshes_.push_back(std::make_unique<Mesh>(entry));
                } else {
                    meshes_.push_back(std::make_unique<Mesh>(
                        allocator, upload,
                        vertices, indices));
                }
            }
        }
    }

    bounding_radius_ = glm::length(local_aabb_.max - local_aabb_.min) * 0.5f;

    // No LOD data for glTF-loaded models — single LOD 0
    LodLevel lod0{};
    lod0.first_mega_entry = 0;
    lod0.mega_entry_count = static_cast<uint32_t>(mega_entries_.size());
    lod0.max_screen_size = 1.0f;
    lod_levels_.push_back(lod0);

    FJELL_GFX_INFO("Model loaded: {} meshes, {} textures, {} meshlets",
                   meshes_.size(), textures_.size(), total_meshlet_count());
}

Model::Model(VmaAllocator allocator, UploadContext& upload,
             const PrimitiveMeshData& data, MegaBuffer* mega,
             MeshletBuffer* meshlet_buf) {
    for (const auto& v : data.vertices) {
        collision_vertices_.push_back(v.position);
        local_aabb_.expand(v.position);
    }
    collision_indices_ = data.indices;

    auto meshlet_result = build_meshlets(
        data.vertices.data(), static_cast<uint32_t>(data.vertices.size()),
        data.indices.data(), static_cast<uint32_t>(data.indices.size()));
    meshlet_results_.push_back(meshlet_result);

    if (mega) {
        auto entry = mega->upload(data.vertices, data.indices);
        if (meshlet_buf && !meshlet_result.meshlets.empty()) {
            auto range = meshlet_buf->upload(meshlet_result, entry.vertex_offset);
            entry.meshlet_offset = range.meshlet_offset;
            entry.meshlet_count = range.meshlet_count;
        }
        mega_entries_.push_back(entry);
        meshes_.push_back(std::make_unique<Mesh>(entry));
    } else {
        meshes_.push_back(std::make_unique<Mesh>(
            allocator, upload,
            data.vertices, data.indices));
    }

    bounding_radius_ = glm::length(local_aabb_.max - local_aabb_.min) * 0.5f;

    // Primitives: single LOD 0
    LodLevel lod0{};
    lod0.first_mega_entry = 0;
    lod0.mega_entry_count = static_cast<uint32_t>(mega_entries_.size());
    lod0.max_screen_size = 1.0f;
    lod_levels_.push_back(lod0);

    // Generate SDF at runtime for primitives (no .fjmesh file)
    // Use 16³ for primitives — they're simple shapes, don't need high resolution
    if (!collision_vertices_.empty() && !collision_indices_.empty()) {
        auto sdf = generate_sdf(
            collision_vertices_.data(),
            static_cast<uint32_t>(collision_vertices_.size()),
            collision_indices_.data(),
            static_cast<uint32_t>(collision_indices_.size()),
            local_aabb_.min, local_aabb_.max, 16);
        sdf_data_ = std::move(sdf.data);
        sdf_resolution_ = sdf.resolution;
        sdf_min_ = sdf.sdf_min;
        sdf_max_ = sdf.sdf_max;
    }
}

void Model::draw(VkCommandBuffer cmd) const {
    for (auto& mesh : meshes_) {
        if (mesh->has_mega_entry()) {
            const auto& entry = mesh->mega_entry();
            vkCmdDrawIndexed(cmd, entry.index_count, 1,
                             entry.first_index,
                             static_cast<int32_t>(entry.vertex_offset), 0);
        } else {
            mesh->bind(cmd);
            mesh->draw(cmd);
        }
    }
}

void Model::draw(VkCommandBuffer cmd, uint32_t first_instance) const {
    for (auto& mesh : meshes_) {
        if (mesh->has_mega_entry()) {
            const auto& entry = mesh->mega_entry();
            vkCmdDrawIndexed(cmd, entry.index_count, 1,
                             entry.first_index,
                             static_cast<int32_t>(entry.vertex_offset),
                             first_instance);
        } else {
            mesh->bind(cmd);
            mesh->draw(cmd);
        }
    }
}

uint32_t Model::total_indices() const {
    uint32_t total = 0;
    for (auto& mesh : meshes_) {
        total += mesh->index_count();
    }
    return total;
}

uint32_t Model::select_lod(float screen_size) const {
    for (uint32_t i = 0; i < static_cast<uint32_t>(lod_levels_.size()); ++i) {
        if (screen_size >= lod_levels_[i].max_screen_size || i == lod_levels_.size() - 1) {
            return i;
        }
    }
    return static_cast<uint32_t>(lod_levels_.size());
}

uint32_t Model::select_lod_sq(float r_proj_sq, float dist_sq) const {
    // screen_size = sqrt(r_proj_sq / dist_sq)
    // threshold comparison: screen_size >= t  <==>  r_proj_sq >= t*t * dist_sq
    for (uint32_t i = 0; i < static_cast<uint32_t>(lod_levels_.size()); ++i) {
        float t = lod_levels_[i].max_screen_size;
        if (r_proj_sq >= t * t * dist_sq || i == lod_levels_.size() - 1) {
            return i;
        }
    }
    return static_cast<uint32_t>(lod_levels_.size());
}

uint32_t Model::total_meshlet_count() const {
    uint32_t total = 0;
    for (const auto& result : meshlet_results_) {
        total += static_cast<uint32_t>(result.meshlets.size());
    }
    return total;
}

// ── .fjmesh loading ─────────────────────────────────────────────────────

FjMeshData Model::load_fjmesh(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        throw std::runtime_error("Failed to open .fjmesh file: " + path);
    }

    // Read header — v1/v2 files have a smaller header (no joint_count/pad_v3)
    FjMeshHeader header{};
    constexpr size_t V2_HEADER_SIZE = offsetof(FjMeshHeader, joint_count);

    // Read enough to get magic+version first
    if (std::fread(&header, V2_HEADER_SIZE, 1, f) != 1) {
        std::fclose(f);
        throw std::runtime_error("Failed to read .fjmesh header: " + path);
    }

    if (header.magic != FJMESH_MAGIC) {
        std::fclose(f);
        throw std::runtime_error("Invalid .fjmesh magic in: " + path);
    }
    if (header.version < 1 || header.version > FJMESH_VERSION) {
        std::fclose(f);
        throw std::runtime_error("Unsupported .fjmesh version " +
            std::to_string(header.version) + " in: " + path);
    }

    // Read v3+ extension fields if present
    if (header.version >= 3) {
        constexpr size_t V3_END = offsetof(FjMeshHeader, sdf_resolution);
        size_t ext_size = V3_END - V2_HEADER_SIZE;
        auto* ext_ptr = reinterpret_cast<char*>(&header) + V2_HEADER_SIZE;
        if (std::fread(ext_ptr, ext_size, 1, f) != 1) {
            std::fclose(f);
            throw std::runtime_error("Failed to read .fjmesh v3 header fields: " + path);
        }
    }
    // Read v4+ SDF fields if present
    if (header.version >= 4) {
        constexpr size_t V4_START = offsetof(FjMeshHeader, sdf_resolution);
        size_t v4_size = sizeof(FjMeshHeader) - V4_START;
        auto* v4_ptr = reinterpret_cast<char*>(&header) + V4_START;
        if (std::fread(v4_ptr, v4_size, 1, f) != 1) {
            std::fclose(f);
            throw std::runtime_error("Failed to read .fjmesh v4 header fields: " + path);
        }
    }
    if (header.flags & FJMESH_FLAG_SKINNED) {
        std::fclose(f);
        FJELL_GFX_WARN("Skipping skinned .fjmesh (use SkeletalModel): {}", path);
        return {};
    }
    if (header.max_meshlet_vertices != MAX_MESHLET_VERTICES ||
        header.max_meshlet_triangles != MAX_MESHLET_TRIANGLES) {
        std::fclose(f);
        throw std::runtime_error("Meshlet limits mismatch in: " + path);
    }

    FjMeshData data;
    data.aabb.min = header.aabb_min;
    data.aabb.max = header.aabb_max;

    auto read_section = [&](auto& vec, uint64_t offset, uint32_t count) {
        using T = typename std::remove_reference_t<decltype(vec)>::value_type;
        vec.resize(count);
        if (count == 0) return;
        std::fseek(f, static_cast<long>(offset), SEEK_SET);
        if (std::fread(vec.data(), sizeof(T), count, f) != count) {
            std::fclose(f);
            throw std::runtime_error("Failed to read .fjmesh section in: " + path);
        }
    };

    read_section(data.vertices, header.vertices_offset, header.vertex_count);
    read_section(data.indices, header.indices_offset, header.index_count);
    read_section(data.submeshes, header.submeshes_offset, header.submesh_count);
    read_section(data.meshlets, header.meshlets_offset, header.meshlet_count);
    read_section(data.bounds, header.meshlet_bounds_offset, header.meshlet_count);
    read_section(data.meshlet_vertices, header.meshlet_vertices_offset, header.meshlet_vertex_count);
    read_section(data.meshlet_triangles, header.meshlet_triangles_offset, header.meshlet_triangle_byte_count);
    read_section(data.collision_vertices, header.collision_vertices_offset, header.collision_vertex_count);
    read_section(data.collision_indices, header.collision_indices_offset, header.collision_index_count);

    // LOD levels: v2 stores them in the file, v1 gets a synthesized single LOD
    if (header.version >= 2 && header.lod_count > 0) {
        read_section(data.lod_levels, header.lod_levels_offset, header.lod_count);
    } else {
        // v1 compatibility: treat all submeshes as LOD 0
        FjMeshLodLevel lod0{};
        lod0.first_submesh = 0;
        lod0.submesh_count = header.submesh_count;
        lod0.max_screen_size = 1.0f;
        data.lod_levels.push_back(lod0);
    }

    // SDF data (v4+)
    if (header.version >= 4 && header.sdf_resolution > 0 && header.sdf_data_offset > 0) {
        uint32_t res = header.sdf_resolution;
        uint32_t voxel_count = res * res * res;
        data.sdf_resolution = res;
        data.sdf_min = header.sdf_min;
        data.sdf_max = header.sdf_max;
        read_section(data.sdf_data, header.sdf_data_offset, voxel_count);
    }

    std::fclose(f);

    FJELL_GFX_DEBUG("Loaded .fjmesh: {} ({} verts, {} indices, {} submeshes, {} meshlets, {} LODs)",
                    path, data.vertices.size(), data.indices.size(),
                    data.submeshes.size(), data.meshlets.size(), data.lod_levels.size());

    return data;
}

Model::Model(VmaAllocator allocator, UploadContext& upload,
             FjMeshData&& data, MegaBuffer* mega,
             MeshletBuffer* meshlet_buf) {
    collision_vertices_ = std::move(data.collision_vertices);
    collision_indices_ = std::move(data.collision_indices);
    local_aabb_ = data.aabb;
    sdf_data_ = std::move(data.sdf_data);
    sdf_resolution_ = data.sdf_resolution;
    sdf_min_ = data.sdf_min;
    sdf_max_ = data.sdf_max;
    bounding_radius_ = glm::length(local_aabb_.max - local_aabb_.min) * 0.5f;

    for (const auto& submesh : data.submeshes) {
        // Build per-submesh vertex and index arrays (rebased to local indices)
        uint32_t vertex_count = 0;
        if (submesh.index_count > 0) {
            // Find the highest index in this submesh to determine vertex count
            uint32_t max_idx = 0;
            for (uint32_t i = 0; i < submesh.index_count; ++i) {
                max_idx = std::max(max_idx, data.indices[submesh.first_index + i]);
            }
            vertex_count = max_idx + 1 - submesh.vertex_offset;
        }

        std::vector<Vertex> sub_vertices(
            data.vertices.begin() + submesh.vertex_offset,
            data.vertices.begin() + submesh.vertex_offset + vertex_count);

        std::vector<uint32_t> sub_indices(
            data.indices.begin() + submesh.first_index,
            data.indices.begin() + submesh.first_index + submesh.index_count);

        for (auto& idx : sub_indices) {
            idx -= submesh.vertex_offset;
        }

        // Rebuild meshlets from the pre-built data for this submesh.
        // The meshlet vertex indices reference global vertices, so rebase them.
        MeshletBuildResult meshlet_result;
        if (submesh.meshlet_count > 0) {
            uint32_t ml_begin = submesh.first_meshlet;
            uint32_t ml_end = ml_begin + submesh.meshlet_count;

            // Find the range of meshlet_vertices[] and meshlet_triangles[] used
            uint32_t mv_begin = data.meshlets[ml_begin].vertex_offset;
            uint32_t mv_end = mv_begin;
            uint32_t mt_begin = data.meshlets[ml_begin].triangle_offset;
            uint32_t mt_end = mt_begin;

            for (uint32_t i = ml_begin; i < ml_end; ++i) {
                mv_end = std::max(mv_end,
                    data.meshlets[i].vertex_offset + data.meshlets[i].vertex_count);
                mt_end = std::max(mt_end,
                    data.meshlets[i].triangle_offset + data.meshlets[i].triangle_count);
            }

            // Copy meshlets, rebasing offsets to local
            for (uint32_t i = ml_begin; i < ml_end; ++i) {
                Meshlet ml = data.meshlets[i];
                ml.vertex_offset -= mv_begin;
                ml.triangle_offset -= mt_begin;
                meshlet_result.meshlets.push_back(ml);
            }

            meshlet_result.bounds.assign(
                data.bounds.begin() + ml_begin,
                data.bounds.begin() + ml_end);

            meshlet_result.meshlet_vertices.assign(
                data.meshlet_vertices.begin() + mv_begin,
                data.meshlet_vertices.begin() + mv_end);
            for (auto& v : meshlet_result.meshlet_vertices) {
                v -= submesh.vertex_offset;
            }

            // Triangle data is stored as 3 bytes per triangle
            meshlet_result.meshlet_triangles.assign(
                data.meshlet_triangles.begin() + mt_begin * 3,
                data.meshlet_triangles.begin() + mt_end * 3);
        }
        meshlet_results_.push_back(meshlet_result);

        if (mega) {
            auto entry = mega->upload(sub_vertices, sub_indices);
            if (meshlet_buf && !meshlet_result.meshlets.empty()) {
                auto range = meshlet_buf->upload(meshlet_result, entry.vertex_offset);
                entry.meshlet_offset = range.meshlet_offset;
                entry.meshlet_count = range.meshlet_count;
            }
            mega_entries_.push_back(entry);
            meshes_.push_back(std::make_unique<Mesh>(entry));
        } else {
            meshes_.push_back(std::make_unique<Mesh>(
                allocator, upload,
                sub_vertices, sub_indices));
        }
    }

    // Build LOD levels from file data, mapping submesh indices to mega_entries
    for (const auto& file_lod : data.lod_levels) {
        LodLevel lod{};
        lod.first_mega_entry = file_lod.first_submesh;
        lod.mega_entry_count = file_lod.submesh_count;
        lod.max_screen_size = file_lod.max_screen_size;
        lod_levels_.push_back(lod);
    }

    // Fallback: if no LOD data, treat everything as LOD 0
    if (lod_levels_.empty()) {
        LodLevel lod0{};
        lod0.first_mega_entry = 0;
        lod0.mega_entry_count = static_cast<uint32_t>(mega_entries_.size());
        lod0.max_screen_size = 1.0f;
        lod_levels_.push_back(lod0);
    }

    FJELL_GFX_INFO("Model loaded from .fjmesh: {} meshes, {} meshlets, {} LODs",
                   meshes_.size(), total_meshlet_count(), lod_levels_.size());
}

} // namespace fjell
