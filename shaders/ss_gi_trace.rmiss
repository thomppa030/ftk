#version 460
#extension GL_EXT_ray_tracing : require

// ── Screen-Space GI: Miss ──────────────────────────────────────────────
// Ray missed all geometry — return zero (sky contribution handled by IBL).

layout(location = 0) rayPayloadInEXT vec4 payload;

void main() {
    payload = vec4(0.0, 0.0, 0.0, -1.0); // w < 0 = miss
}
