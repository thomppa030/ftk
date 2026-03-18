#include "renderer/model.hpp"
#include "renderer/meshlet.hpp"
#include "renderer/meshlet_buffer.hpp"
#include "renderer/primitives.hpp"
#include "renderer/vertex.hpp"
#include "core/log.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/glm_element_traits.hpp>

#include <stb_image.h>

#include <filesystem>
#include <stdexcept>
#include <unordered_map>

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

Model::Model(VkDevice device, VkPhysicalDevice physical_device,
             VkCommandPool command_pool, VkQueue queue,
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
            device, physical_device, command_pool, queue,
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
                        device, physical_device, command_pool, queue,
                        vertices, indices));
                }
            }
        }
    }

    FJELL_GFX_INFO("Model loaded: {} meshes, {} textures, {} meshlets",
                   meshes_.size(), textures_.size(), total_meshlet_count());
}

Model::Model(VkDevice device, VkPhysicalDevice physical_device,
             VkCommandPool command_pool, VkQueue queue,
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
            device, physical_device, command_pool, queue,
            data.vertices, data.indices));
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

uint32_t Model::total_indices() const {
    uint32_t total = 0;
    for (auto& mesh : meshes_) {
        total += mesh->index_count();
    }
    return total;
}

uint32_t Model::total_meshlet_count() const {
    uint32_t total = 0;
    for (const auto& result : meshlet_results_) {
        total += static_cast<uint32_t>(result.meshlets.size());
    }
    return total;
}

} // namespace fjell
