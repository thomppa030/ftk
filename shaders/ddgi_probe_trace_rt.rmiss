#version 460
#extension GL_EXT_ray_tracing : require

layout(set = 0, binding = 3) uniform samplerCube sky_cubemap;

layout(location = 0) rayPayloadInEXT vec4 payload;

void main() {
    // Sample blurred sky for probes — mip 0 has extreme HDR peaks (sun disk)
    // that drown out surface bounce color bleeding in the irradiance update.
    vec3 sky = textureLod(sky_cubemap, gl_WorldRayDirectionEXT, 4.0).rgb;
    payload = vec4(sky, gl_RayTmaxEXT);
}
