#include "gpu/vulkan/access.hpp"

namespace fjell::gpu::vulkan {

namespace {

constexpr VkPipelineStageFlags2 FRAGMENT_TESTS =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

// A sampled image is in the read-only layout of its aspect.
VkImageLayout sampled_layout(bool depth) {
    return depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

} // namespace

ImageScope image_scope(Access access, bool depth) {
    switch (access) {
        // An attachment's load op reads what is there before the pass writes
        // over it.
        case Access::color_attachment:
            return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
        case Access::depth_attachment:
            return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, FRAGMENT_TESTS,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        // A multisample resolve, depth or colour, writes at the colour
        // attachment output stage as a colour attachment write.
        case Access::depth_resolve:
            return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
        case Access::depth_attachment_read:
        case Access::input_attachment:
            return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, FRAGMENT_TESTS,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT};
        case Access::depth_read_sampled:
            return {VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    FRAGMENT_TESTS | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        case Access::sampled_fragment:
            return {sampled_layout(depth), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        case Access::sampled_vertex:
            return {sampled_layout(depth), VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        case Access::sampled_mesh:
            return {sampled_layout(depth),
                    VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        case Access::sampled_compute:
            return {sampled_layout(depth), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        case Access::sampled_raytracing:
            return {sampled_layout(depth), VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        // A storage image is only ever read in GENERAL, whatever the access:
        // imageLoad through a read-only layout is invalid.
        case Access::storage_read_compute:
            return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        case Access::storage_write_compute:
            return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
        case Access::storage_read_write_compute:
            return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
        case Access::storage_write_raytracing:
            return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
        case Access::copy_src:
            return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT};
        case Access::copy_dst:
            return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT};
        case Access::clear:
            return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT};
        default:
            return {};
    }
}

std::optional<ImageScope> image_scope(AccessSet accesses, bool depth) {
    ImageScope merged;
    bool first = true;
    bool agree = true;
    accesses.for_each([&](Access access) {
        const ImageScope scope = image_scope(access, depth);
        if (first) {
            merged.layout = scope.layout;
            first = false;
        } else if (scope.layout != merged.layout) {
            agree = false;
        }
        merged.stages |= scope.stages;
        merged.access |= scope.access;
    });
    if (!agree) return std::nullopt;
    return merged;
}

ImageScope merged_image_scope(AccessSet accesses, bool depth) {
    auto is_attachment = [](VkImageLayout layout) {
        return layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
               layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL ||
               layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
               layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    };
    ImageScope merged;
    accesses.for_each([&](Access access) {
        const ImageScope scope = image_scope(access, depth);
        const VkImageLayout a = merged.layout;
        const VkImageLayout b = scope.layout;
        if (a == VK_IMAGE_LAYOUT_UNDEFINED || a == b) {
            merged.layout = b;
        } else if (b != VK_IMAGE_LAYOUT_UNDEFINED && !is_attachment(a)) {
            merged.layout = is_attachment(b) ? b : VK_IMAGE_LAYOUT_GENERAL;
        }
        merged.stages |= scope.stages;
        merged.access |= scope.access;
    });
    return merged;
}

BufferScope buffer_scope(Access access) {
    switch (access) {
        case Access::uniform_read:
            return {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_UNIFORM_READ_BIT};
        case Access::storage_buffer_read_compute:
            return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        case Access::storage_buffer_read_vertex:
            return {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        case Access::storage_buffer_read_fragment:
            return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        case Access::storage_buffer_read_mesh:
            return {VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        case Access::indirect_read:
            return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
        case Access::index_read:
            return {VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT};
        case Access::vertex_read:
            return {VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                    VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT};
        case Access::storage_buffer_write_compute:
            return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
        case Access::storage_buffer_read_write_compute:
            return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
        case Access::copy_src:
            return {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
        case Access::copy_dst:
            return {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
        case Access::clear:
            return {VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
        default:
            return {};
    }
}

BufferScope buffer_scope(AccessSet accesses) {
    BufferScope merged;
    accesses.for_each([&](Access access) {
        const BufferScope scope = buffer_scope(access);
        merged.stages |= scope.stages;
        merged.access |= scope.access;
    });
    return merged;
}

std::string layout_name(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:                        return "UNDEFINED";
        case VK_IMAGE_LAYOUT_GENERAL:                          return "GENERAL";
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:         return "COLOR_ATT";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DS_ATT";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:  return "DS_RO";
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:         return "SHADER_RO";
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:             return "XFER_SRC";
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:             return "XFER_DST";
        case VK_IMAGE_LAYOUT_PREINITIALIZED:                   return "PREINIT";
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:         return "DEPTH_ATT";
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:           return "DEPTH_RO";
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:                  return "PRESENT";
        default:                                               return std::to_string(static_cast<int>(layout));
    }
}

VkPipelineStageFlags2 compute_queue_stages(VkPipelineStageFlags2 stages) {
    constexpr VkPipelineStageFlags2 GRAPHICS_ONLY =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | FRAGMENT_TESTS |
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
        VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
        VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
        VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
        VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
        VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT;

    if ((stages & GRAPHICS_ONLY) == 0) return stages;
    return (stages & ~GRAPHICS_ONLY) | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

} // namespace fjell::gpu::vulkan
