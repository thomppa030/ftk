#version 460
#extension GL_EXT_ray_tracing : require

layout(set = 0, binding = 3) uniform samplerCube sky_cubemap;

layout(location = 0) rayPayloadInEXT vec4 payload;

void main() {
    vec3 sky = textureLod(sky_cubemap, gl_WorldRayDirectionEXT, 0.0).rgb;
    payload = vec4(sky, gl_RayTmaxEXT);
}
