#include "gpu/vulkan/translate.hpp"

namespace fjell::gpu::vulkan {

// The switches below have no default on purpose: a value added to the
// interface without a Vulkan mapping fails the build (-Wswitch) instead of
// mapping to nothing at run time.

VkFormat to_vk(Format format) {
    switch (format) {
        case Format::undefined:         return VK_FORMAT_UNDEFINED;
        case Format::r8_unorm:          return VK_FORMAT_R8_UNORM;
        case Format::rg8_unorm:         return VK_FORMAT_R8G8_UNORM;
        case Format::rgba8_unorm:       return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::rgba8_srgb:        return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::bgra8_unorm:       return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::bgra8_srgb:        return VK_FORMAT_B8G8R8A8_SRGB;
        case Format::r16_unorm:         return VK_FORMAT_R16_UNORM;
        case Format::rg16_unorm:        return VK_FORMAT_R16G16_UNORM;
        case Format::rgba16_unorm:      return VK_FORMAT_R16G16B16A16_UNORM;
        case Format::r16_float:         return VK_FORMAT_R16_SFLOAT;
        case Format::rg16_float:        return VK_FORMAT_R16G16_SFLOAT;
        case Format::rgba16_float:      return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::r32_float:         return VK_FORMAT_R32_SFLOAT;
        case Format::r32_uint:          return VK_FORMAT_R32_UINT;
        case Format::rgba32_float:      return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::d32_float:         return VK_FORMAT_D32_SFLOAT;
        case Format::d32_float_s8_uint: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

Format from_vk(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_UNORM:            return Format::r8_unorm;
        case VK_FORMAT_R8G8_UNORM:          return Format::rg8_unorm;
        case VK_FORMAT_R8G8B8A8_UNORM:      return Format::rgba8_unorm;
        case VK_FORMAT_R8G8B8A8_SRGB:       return Format::rgba8_srgb;
        case VK_FORMAT_B8G8R8A8_UNORM:      return Format::bgra8_unorm;
        case VK_FORMAT_B8G8R8A8_SRGB:       return Format::bgra8_srgb;
        case VK_FORMAT_R16_UNORM:           return Format::r16_unorm;
        case VK_FORMAT_R16G16_UNORM:        return Format::rg16_unorm;
        case VK_FORMAT_R16G16B16A16_UNORM:  return Format::rgba16_unorm;
        case VK_FORMAT_R16_SFLOAT:          return Format::r16_float;
        case VK_FORMAT_R16G16_SFLOAT:       return Format::rg16_float;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return Format::rgba16_float;
        case VK_FORMAT_R32_SFLOAT:          return Format::r32_float;
        case VK_FORMAT_R32_UINT:            return Format::r32_uint;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return Format::rgba32_float;
        case VK_FORMAT_D32_SFLOAT:          return Format::d32_float;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:  return Format::d32_float_s8_uint;
        default:                            return Format::undefined;
    }
}

VkFormat to_vk(VertexFormat format) {
    switch (format) {
        case VertexFormat::float2: return VK_FORMAT_R32G32_SFLOAT;
        case VertexFormat::float3: return VK_FORMAT_R32G32B32_SFLOAT;
        case VertexFormat::float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case VertexFormat::uint4:  return VK_FORMAT_R32G32B32A32_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkSampleCountFlagBits to_vk(Samples samples) {
    switch (samples) {
        case Samples::x1: return VK_SAMPLE_COUNT_1_BIT;
        case Samples::x2: return VK_SAMPLE_COUNT_2_BIT;
        case Samples::x4: return VK_SAMPLE_COUNT_4_BIT;
        case Samples::x8: return VK_SAMPLE_COUNT_8_BIT;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

VkImageUsageFlags to_vk(TextureUses uses) {
    VkImageUsageFlags flags = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    uses.for_each([&](TextureUse use) {
        switch (use) {
            case TextureUse::sampled:      flags |= VK_IMAGE_USAGE_SAMPLED_BIT; break;
            case TextureUse::storage:      flags |= VK_IMAGE_USAGE_STORAGE_BIT; break;
            case TextureUse::color_target: flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; break;
            case TextureUse::depth_target: flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; break;
        }
    });
    return flags;
}

VkBufferUsageFlags to_vk(BufferUses uses) {
    VkBufferUsageFlags flags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    uses.for_each([&](BufferUse use) {
        switch (use) {
            case BufferUse::uniform:  flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; break;
            case BufferUse::storage:  flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; break;
            case BufferUse::vertex:   flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; break;
            case BufferUse::index:    flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT; break;
            case BufferUse::indirect: flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT; break;
            case BufferUse::acceleration_input:
                flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
                break;
            case BufferUse::device_address:
                flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
                break;
        }
    });
    return flags;
}

VkClearValue to_vk(const Clear& clear, Format format) {
    VkClearValue value{};
    switch (kind(format)) {
        case FormatKind::depth:
        case FormatKind::depth_stencil:
            value.depthStencil = {clear.depth, clear.stencil};
            break;
        case FormatKind::color_uint:
            for (size_t i = 0; i < clear.color.size(); ++i) {
                value.color.uint32[i] = static_cast<uint32_t>(clear.color[i]);
            }
            break;
        case FormatKind::color:
        case FormatKind::none:
            for (size_t i = 0; i < clear.color.size(); ++i) {
                value.color.float32[i] = clear.color[i];
            }
            break;
    }
    return value;
}

VkFilter to_vk(Filter filter) {
    switch (filter) {
        case Filter::nearest: return VK_FILTER_NEAREST;
        case Filter::linear:  return VK_FILTER_LINEAR;
    }
    return VK_FILTER_LINEAR;
}

VkSamplerMipmapMode to_vk_mipmap(Filter filter) {
    switch (filter) {
        case Filter::nearest: return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        case Filter::linear:  return VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }
    return VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

VkSamplerAddressMode to_vk(Address address) {
    switch (address) {
        case Address::repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case Address::mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case Address::clamp:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case Address::border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkBorderColor to_vk(Border border) {
    switch (border) {
        case Border::transparent_black: return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        case Border::opaque_black:      return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        case Border::opaque_white:      return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }
    return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
}

VkCompareOp to_vk(Compare compare) {
    switch (compare) {
        case Compare::never:         return VK_COMPARE_OP_NEVER;
        case Compare::less:          return VK_COMPARE_OP_LESS;
        case Compare::equal:         return VK_COMPARE_OP_EQUAL;
        case Compare::less_equal:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case Compare::greater:       return VK_COMPARE_OP_GREATER;
        case Compare::not_equal:     return VK_COMPARE_OP_NOT_EQUAL;
        case Compare::greater_equal: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case Compare::always:        return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkImageViewType to_vk(ViewKind kind) {
    switch (kind) {
        case ViewKind::automatic:
        case ViewKind::tex2d:       return VK_IMAGE_VIEW_TYPE_2D;
        case ViewKind::tex2d_array: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case ViewKind::cube:        return VK_IMAGE_VIEW_TYPE_CUBE;
        case ViewKind::tex3d:       return VK_IMAGE_VIEW_TYPE_3D;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}

VkImageAspectFlags image_aspects(Format format) {
    switch (kind(format)) {
        case FormatKind::depth:         return VK_IMAGE_ASPECT_DEPTH_BIT;
        case FormatKind::depth_stencil: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        case FormatKind::color:
        case FormatKind::color_uint:
        case FormatKind::none:          return VK_IMAGE_ASPECT_COLOR_BIT;
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

VkImageAspectFlags view_aspect(Format format) {
    switch (kind(format)) {
        case FormatKind::depth:
        case FormatKind::depth_stencil: return VK_IMAGE_ASPECT_DEPTH_BIT;
        case FormatKind::color:
        case FormatKind::color_uint:
        case FormatKind::none:          return VK_IMAGE_ASPECT_COLOR_BIT;
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

VkShaderStageFlags to_vk(ShaderStages stages) {
    VkShaderStageFlags flags = 0;
    stages.for_each([&](ShaderStage stage) {
        switch (stage) {
            case ShaderStage::vertex:   flags |= VK_SHADER_STAGE_VERTEX_BIT; break;
            case ShaderStage::fragment: flags |= VK_SHADER_STAGE_FRAGMENT_BIT; break;
            case ShaderStage::compute:  flags |= VK_SHADER_STAGE_COMPUTE_BIT; break;
            case ShaderStage::task:     flags |= VK_SHADER_STAGE_TASK_BIT_EXT; break;
            case ShaderStage::mesh:     flags |= VK_SHADER_STAGE_MESH_BIT_EXT; break;
        }
    });
    return flags;
}

VkDescriptorType to_vk(BindingKind kind) {
    switch (kind) {
        case BindingKind::uniform_buffer:         return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case BindingKind::storage_buffer:         return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case BindingKind::sampled_texture:        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case BindingKind::texture:                return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case BindingKind::sampler:                return VK_DESCRIPTOR_TYPE_SAMPLER;
        case BindingKind::storage_texture:        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case BindingKind::acceleration_structure: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

VkBlendFactor to_vk(BlendFactor factor) {
    switch (factor) {
        case BlendFactor::zero:                return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::one:                 return VK_BLEND_FACTOR_ONE;
        case BlendFactor::src_alpha:           return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::one_minus_src_alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::dst_color:           return VK_BLEND_FACTOR_DST_COLOR;
        case BlendFactor::dst_alpha:           return VK_BLEND_FACTOR_DST_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkPrimitiveTopology to_vk(Topology topology) {
    switch (topology) {
        case Topology::triangles:      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case Topology::triangle_strip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case Topology::lines:          return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkCullModeFlags to_vk(Cull cull) {
    switch (cull) {
        case Cull::none:  return VK_CULL_MODE_NONE;
        case Cull::back:  return VK_CULL_MODE_BACK_BIT;
        case Cull::front: return VK_CULL_MODE_FRONT_BIT;
    }
    return VK_CULL_MODE_NONE;
}

VkFrontFace to_vk(FrontFace front_face) {
    switch (front_face) {
        case FrontFace::counter_clockwise: return VK_FRONT_FACE_COUNTER_CLOCKWISE;
        case FrontFace::clockwise:         return VK_FRONT_FACE_CLOCKWISE;
    }
    return VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

VkPolygonMode to_vk(Fill fill) {
    switch (fill) {
        case Fill::solid: return VK_POLYGON_MODE_FILL;
        case Fill::lines: return VK_POLYGON_MODE_LINE;
    }
    return VK_POLYGON_MODE_FILL;
}

VkColorComponentFlags to_vk(ChannelMask channels) {
    VkColorComponentFlags flags = 0;
    channels.for_each([&](Channel channel) {
        switch (channel) {
            case Channel::r: flags |= VK_COLOR_COMPONENT_R_BIT; break;
            case Channel::g: flags |= VK_COLOR_COMPONENT_G_BIT; break;
            case Channel::b: flags |= VK_COLOR_COMPONENT_B_BIT; break;
            case Channel::a: flags |= VK_COLOR_COMPONENT_A_BIT; break;
        }
    });
    return flags;
}

} // namespace fjell::gpu::vulkan
