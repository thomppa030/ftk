#include "renderer/resources/skeletal_model.hpp"
#include "gpu/device.hpp"
#include "gpu/vulkan/native.hpp"
#include "renderer/resources/fjmesh.hpp"
#include "renderer/resources/meshlet_buffer.hpp"
#include "core/log.hpp"

#include <cstddef>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace fjell {

SkeletalModel::SkeletalModel(VmaAllocator allocator, gpu::Device& gpu,
                             FjSkinnedMeshData&& data,
                             MeshletBuffer* meshlet_buf)
    : vertex_buffer_{allocator,
                     data.vertices.size() * sizeof(SkinnedVertex),
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                         | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VMA_MEMORY_USAGE_AUTO, 0, 0, gpu::vulkan::upload_families(gpu)}
    , index_buffer_{allocator,
                    data.indices.size() * sizeof(uint32_t),
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    VMA_MEMORY_USAGE_AUTO, 0, 0, gpu::vulkan::upload_families(gpu)}
    , index_count_{static_cast<uint32_t>(data.indices.size())}
    , joint_count_{data.joint_count}
    , local_aabb_{data.aabb}
    , collision_vertices_{std::move(data.collision_vertices)}
    , collision_indices_{std::move(data.collision_indices)}
    , surface_indices_{data.indices} {

    surface_positions_.reserve(data.vertices.size());
    surface_influences_.reserve(data.vertices.size());
    for (const auto& v : data.vertices) {
        surface_positions_.push_back(v.position);
        surface_influences_.push_back({v.bone_ids, v.bone_weights});
    }

    glm::vec3 center = (local_aabb_.min + local_aabb_.max) * 0.5f;
    bounding_radius_ = glm::length(local_aabb_.max - center);

    // Build meshlets for the GPU-driven mesh-shader path. meshopt only
    // walks position + stride so it handles SkinnedVertex the same as
    // the static Vertex type.
    if (!data.vertices.empty() && !data.indices.empty()) {
        meshlets_ = build_meshlets(data.vertices.data(),
                                   static_cast<uint32_t>(data.vertices.size()),
                                   data.indices.data(),
                                   static_cast<uint32_t>(data.indices.size()));

        // Upload meshlet records into the shared GPU buffer. The skinned
        // mesh shader fetches verts from this model's own vertex_buffer_
        // (per-draw bound at set 3 binding 4), so vertex_offset stays 0.
        if (meshlet_buf && !meshlets_.meshlets.empty()) {
            meshlet_range_ = meshlet_buf->upload(meshlets_, 0);
        }
    }

    vertex_buffer_size_ = data.vertices.size() * sizeof(SkinnedVertex);

    // Build embedded skeleton from joint data if present
    if (!data.joints.empty()) {
        auto skel_result = Skeleton::from_joints(data.joints);
        if (skel_result) {
            // Resolve each collider's bone now the joint list is final. A name
            // that no longer exists leaves the shape unparented rather than
            // attaching it to joint 0, which would be plausible and wrong.
            skel_result->colliders() = std::move(data.colliders);
            for (auto& col : skel_result->colliders()) {
                col.shape.bone = skel_result->find_joint(col.bone_name);
                if (col.shape.bone < 0 && !col.bone_name.empty()) {
                    FJELL_CORE_WARN("Skeleton: collider references unknown bone '{}'",
                                    col.bone_name);
                }
            }
            skeleton_ = std::make_shared<Skeleton>(std::move(*skel_result));
        } else {
            FJELL_CORE_ERROR("Failed to build skeleton: {}", skel_result.error());
        }
    }

    vertex_buffer_.share(gpu);
    index_buffer_.share(gpu);
    gpu.upload().to_buffer(vertex_buffer_.shared(), 0, data.vertices);
    gpu.upload().to_buffer(index_buffer_.shared(), 0, data.indices);

    FJELL_GFX_DEBUG("SkeletalModel created: {} verts, {} indices, {} joints{}",
                    data.vertices.size(), data.indices.size(), joint_count_,
                    skeleton_ ? " (with skeleton)" : "");
}

void SkeletalModel::pose_surface(const std::vector<glm::mat4>& skin_matrices,
                                 std::vector<glm::vec3>& out) const {
    out.resize(surface_positions_.size());
    if (skin_matrices.empty()) {
        std::copy(surface_positions_.begin(), surface_positions_.end(), out.begin());
        return;
    }

    // Same weighted sum the skinning vertex shader computes, so the posed
    // surface lands exactly where the rendered one does.
    for (size_t i = 0; i < surface_positions_.size(); ++i) {
        const glm::vec4 p{surface_positions_[i], 1.0f};
        const auto& inf = surface_influences_[i];
        glm::vec3 posed{0.0f};
        for (int k = 0; k < 4; ++k) {
            const float w = inf.bone_weights[k];
            const uint32_t bone = inf.bone_ids[k];
            if (w == 0.0f || bone >= skin_matrices.size()) continue;
            posed += glm::vec3(skin_matrices[bone] * p) * w;
        }
        out[i] = posed;
    }
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

    // v1 files end where the v2 fields begin, so the header is read in two
    // parts rather than one sizeof() that would run past the end of an older
    // file and into its geometry.
    FjSkelHeader header{};
    constexpr size_t V1_SIZE = offsetof(FjSkelHeader, collider_count);
    if (std::fread(&header, V1_SIZE, 1, f) != 1) {
        std::fclose(f);
        throw std::runtime_error("Failed to read .fjskel header: " + path);
    }
    if (header.version >= 2) {
        auto* v2_ptr = reinterpret_cast<char*>(&header) + V1_SIZE;
        if (std::fread(v2_ptr, sizeof(FjSkelHeader) - V1_SIZE, 1, f) != 1) {
            std::fclose(f);
            throw std::runtime_error("Failed to read .fjskel v2 header fields: " + path);
        }
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

        // Bone colliders (v2+), named out of the same table as the joints.
        if (header.version >= 2 && header.collider_count > 0 &&
            header.colliders_offset > 0) {
            std::vector<FjSkelCollider> bin_colliders(header.collider_count);
            std::fseek(f, static_cast<long>(header.colliders_offset), SEEK_SET);
            if (std::fread(bin_colliders.data(), sizeof(FjSkelCollider),
                           header.collider_count, f) != header.collider_count) {
                std::fclose(f);
                throw std::runtime_error("Failed to read .fjskel colliders: " + path);
            }

            data.colliders.reserve(bin_colliders.size());
            for (const auto& src : bin_colliders) {
                BoneCollider col;
                col.shape.kind = static_cast<CollisionPrimitiveKind>(src.kind);
                col.shape.position = {src.position[0], src.position[1], src.position[2]};
                col.shape.rotation = glm::quat(src.rotation[3], src.rotation[0],
                                               src.rotation[1], src.rotation[2]);
                col.shape.extents = {src.extents[0], src.extents[1], src.extents[2]};
                if (src.bone_name_length > 0 &&
                    src.bone_name_offset + src.bone_name_length <= header.name_table_size) {
                    col.bone_name = std::string(name_table.data() + src.bone_name_offset,
                                                src.bone_name_length);
                }
                data.colliders.push_back(std::move(col));
            }
        }
    }

    std::fclose(f);

    FJELL_GFX_DEBUG("Loaded .fjskel: {} ({} verts, {} indices, {} joints)",
                    path, data.vertices.size(), data.indices.size(), data.joint_count);
    return data;
}

} // namespace fjell
