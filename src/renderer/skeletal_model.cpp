#include "renderer/skeletal_model.hpp"
#include "renderer/fjmesh.hpp"
#include "core/log.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace fjell {

SkeletalModel::SkeletalModel(VkDevice device, VkPhysicalDevice physical_device,
                             VkCommandPool command_pool, VkQueue transfer_queue,
                             FjSkinnedMeshData&& data)
    : vertex_buffer_{device, physical_device,
                     data.vertices.size() * sizeof(SkinnedVertex),
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT}
    , index_buffer_{device, physical_device,
                    data.indices.size() * sizeof(uint32_t),
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT}
    , index_count_{static_cast<uint32_t>(data.indices.size())}
    , joint_count_{data.joint_count}
    , local_aabb_{data.aabb}
    , collision_vertices_{std::move(data.collision_vertices)}
    , collision_indices_{std::move(data.collision_indices)} {

    glm::vec3 center = (local_aabb_.min + local_aabb_.max) * 0.5f;
    bounding_radius_ = glm::length(local_aabb_.max - center);

    // Build embedded skeleton from joint data if present
    if (!data.joints.empty()) {
        auto skel_result = Skeleton::from_joints(data.joints);
        if (skel_result) {
            skeleton_ = std::make_shared<Skeleton>(std::move(*skel_result));
        } else {
            FJELL_CORE_ERROR("Failed to build skeleton: {}", skel_result.error());
        }
    }

    // Stage and upload vertex data
    VkDeviceSize vert_size = data.vertices.size() * sizeof(SkinnedVertex);
    Buffer vert_staging{device, physical_device, vert_size,
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    vert_staging.write(data.vertices.data(), vert_size);
    Buffer::copy(device, command_pool, transfer_queue,
                 vert_staging, vertex_buffer_, vert_size);

    // Stage and upload index data
    VkDeviceSize idx_size = data.indices.size() * sizeof(uint32_t);
    Buffer idx_staging{device, physical_device, idx_size,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    idx_staging.write(data.indices.data(), idx_size);
    Buffer::copy(device, command_pool, transfer_queue,
                 idx_staging, index_buffer_, idx_size);

    FJELL_GFX_DEBUG("SkeletalModel created: {} verts, {} indices, {} joints{}",
                    data.vertices.size(), data.indices.size(), joint_count_,
                    skeleton_ ? " (with skeleton)" : "");
}

void SkeletalModel::bind(VkCommandBuffer cmd) const {
    VkBuffer buffers[] = {vertex_buffer_.handle()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, index_buffer_.handle(), 0, VK_INDEX_TYPE_UINT32);
}

void SkeletalModel::draw(VkCommandBuffer cmd) const {
    vkCmdDrawIndexed(cmd, index_count_, 1, 0, 0, 0);
}

// ── .fjskel loading (mesh + skeleton) ───────────────────────────────────

FjSkinnedMeshData SkeletalModel::load_fjskel(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        throw std::runtime_error("Failed to open .fjskel: " + path);
    }

    // Read and validate header
    uint32_t magic{0};
    if (std::fread(&magic, sizeof(magic), 1, f) != 1) {
        std::fclose(f);
        throw std::runtime_error("Failed to read .fjskel magic: " + path);
    }
    std::rewind(f);

    if (magic != FJSKEL_MAGIC) {
        std::fclose(f);
        throw std::runtime_error("Invalid .fjskel file (expected FSKL magic): " + path);
    }

    FjSkelHeader header{};
    if (std::fread(&header, sizeof(header), 1, f) != 1) {
        std::fclose(f);
        throw std::runtime_error("Failed to read .fjskel header: " + path);
    }

    FjSkinnedMeshData data;
    data.aabb.min = header.aabb_min;
    data.aabb.max = header.aabb_max;
    data.joint_count = header.joint_count;

    auto read_section = [&](auto& vec, uint64_t offset, uint32_t count) {
        using T = typename std::remove_reference_t<decltype(vec)>::value_type;
        vec.resize(count);
        if (count == 0) return;
        std::fseek(f, static_cast<long>(offset), SEEK_SET);
        if (std::fread(vec.data(), sizeof(T), count, f) != count) {
            std::fclose(f);
            throw std::runtime_error("Failed to read .fjskel section: " + path);
        }
    };

    read_section(data.vertices, header.vertices_offset, header.vertex_count);
    read_section(data.indices, header.indices_offset, header.index_count);
    read_section(data.collision_vertices, header.collision_vertices_offset, header.collision_vertex_count);
    read_section(data.collision_indices, header.collision_indices_offset, header.collision_index_count);

    // Read skeleton joints
    if (header.joint_count > 0 && header.joints_offset > 0) {
        std::vector<FjSkelJointBin> bin_joints(header.joint_count);
        std::fseek(f, static_cast<long>(header.joints_offset), SEEK_SET);
        if (std::fread(bin_joints.data(), sizeof(FjSkelJointBin), header.joint_count, f) != header.joint_count) {
            std::fclose(f);
            throw std::runtime_error("Failed to read .fjskel joints: " + path);
        }

        // Read name table
        std::vector<char> name_table(header.name_table_size);
        if (header.name_table_size > 0) {
            std::fseek(f, static_cast<long>(header.name_table_offset), SEEK_SET);
            if (std::fread(name_table.data(), 1, header.name_table_size, f) != header.name_table_size) {
                std::fclose(f);
                throw std::runtime_error("Failed to read .fjskel name table: " + path);
            }
        }

        // Convert to FjSkelJoint
        data.joints.resize(header.joint_count);
        for (uint32_t i = 0; i < header.joint_count; ++i) {
            auto& src = bin_joints[i];
            auto& dst = data.joints[i];
            dst.parent = src.parent;
            dst.translation = {src.translation[0], src.translation[1], src.translation[2]};
            dst.rotation = glm::quat(src.rotation[3], src.rotation[0],
                                     src.rotation[1], src.rotation[2]); // w,x,y,z
            dst.scale = {src.scale[0], src.scale[1], src.scale[2]};
            if (src.name_length > 0 && src.name_offset + src.name_length <= header.name_table_size) {
                dst.name = std::string(name_table.data() + src.name_offset, src.name_length);
            }
        }
    }

    std::fclose(f);

    FJELL_GFX_DEBUG("Loaded .fjskel: {} ({} verts, {} indices, {} joints)",
                    path, data.vertices.size(), data.indices.size(), data.joint_count);
    return data;
}

} // namespace fjell
