#include "renderer/model.hpp"
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
             const std::string& path) {
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
                meshes_.push_back(std::make_unique<Mesh>(
                    device, physical_device, command_pool, queue,
                    vertices, indices));
            }
        }
    }

    FJELL_GFX_INFO("Model loaded: {} meshes, {} textures",
                   meshes_.size(), textures_.size());
}

Model::Model(VkDevice device, VkPhysicalDevice physical_device,
             VkCommandPool command_pool, VkQueue queue,
             const PrimitiveMeshData& data) {
    meshes_.push_back(std::make_unique<Mesh>(
        device, physical_device, command_pool, queue,
        data.vertices, data.indices));
}

void Model::draw(VkCommandBuffer cmd) const {
    for (auto& mesh : meshes_) {
        mesh->bind(cmd);
        mesh->draw(cmd);
    }
}

uint32_t Model::total_indices() const {
    uint32_t total = 0;
    for (auto& mesh : meshes_) {
        total += mesh->index_count();
    }
    return total;
}

} // namespace fjell
