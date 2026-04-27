#version 460
#extension GL_EXT_ray_tracing : require

// Reflection ray missed all geometry — sample the sky cubemap.

layout(set = 0, binding = 5) uniform samplerCube sky_cubemap;

layout(location = 0) rayPayloadInEXT vec4 payload;

void main() {
    vec3 sky = textureLod(sky_cubemap, normalize(gl_WorldRayDirectionEXT), 0.0).rgb;
    payload = vec4(sky, -1.0); // w < 0 = miss (composite still uses the radiance)
}
