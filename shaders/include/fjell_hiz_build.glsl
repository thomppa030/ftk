#ifndef FJELL_HIZ_BUILD_GLSL
#define FJELL_HIZ_BUILD_GLSL

// Hi-Z pyramid build: max-pool depth into successive mip levels. The
// including shader defines HIZ_SOURCE as the sampler type it reduces from:
// sampler2D for the depth or the previous mip, sampler2DMS for multisampled
// depth, of which texelFetch reads sample 0, the one the depth resolve
// keeps.

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform HIZ_SOURCE src_depth;
layout(set = 0, binding = 1, r32f) uniform writeonly image2D dst_mip;

layout(push_constant) uniform PushConstants {
    uint src_width;
    uint src_height;
    uint dst_width;
    uint dst_height;
};

void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(dst_width) || coord.y >= int(dst_height)) return;

    // Sample 2x2 block from source, take max (farthest depth for conservative test)
    ivec2 src = coord * 2;
    ivec2 src_max = ivec2(int(src_width) - 1, int(src_height) - 1);
    float d00 = texelFetch(src_depth, src, 0).r;
    float d10 = texelFetch(src_depth, min(src + ivec2(1, 0), src_max), 0).r;
    float d01 = texelFetch(src_depth, min(src + ivec2(0, 1), src_max), 0).r;
    float d11 = texelFetch(src_depth, min(src + ivec2(1, 1), src_max), 0).r;

    float result = max(max(d00, d10), max(d01, d11));
    imageStore(dst_mip, coord, vec4(result));
}

#endif // FJELL_HIZ_BUILD_GLSL
