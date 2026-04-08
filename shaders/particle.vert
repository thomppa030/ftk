#version 460

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 view;
    mat4 projection;
    vec4 light_direction;
    vec4 light_color;
    vec4 camera_position;
    vec4 ambient_color;
    mat4 cascade_vp[3];
    vec4 cascade_splits;
    uvec4 tile_info;
    mat4 inv_projection;
    mat4 inv_view_proj;
    mat4 prev_view_proj;
    float time;
} ubo;

struct Particle {
    vec3  position;
    float lifetime;
    vec3  velocity;
    float max_lifetime;
    vec4  color;
    float size;
    float size_end;
    uint  emitter_index;
    float stretch;
    float rotation;
    float rotation_speed;
    uint  texture_index;
    uint  flipbook_params;
};

layout(std430, set = 1, binding = 0) readonly buffer ParticleBuffer {
    Particle particles[];
};

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out vec4 frag_color;
layout(location = 2) out flat uint frag_texture_index;
layout(location = 3) out vec3 frag_world_pos;
layout(location = 4) out vec3 frag_world_normal;
layout(location = 5) out flat uint frag_is_mesh;

void main() {
    // base_offset comes from firstInstance in VkDrawIndirectCommand
    uint base_offset = gl_InstanceIndex;
    uint particle_index = base_offset + gl_VertexIndex / 6;
    uint corner = gl_VertexIndex % 6;

    Particle p = particles[particle_index];

    // Dead particle — degenerate triangle
    if (p.lifetime <= 0.0) {
        gl_Position = vec4(0.0);
        frag_color = vec4(0.0);
        frag_uv = vec2(0.0);
        return;
    }

    // Two triangles forming a quad:
    // 0--1    triangle 0: 0,1,2
    // |\ |    triangle 1: 2,1,3
    // | \|
    // 2--3
    vec2 offsets[6] = vec2[](
        vec2(-1, -1), vec2( 1, -1), vec2(-1,  1),
        vec2(-1,  1), vec2( 1, -1), vec2( 1,  1)
    );
    vec2 uvs[6] = vec2[](
        vec2(0, 0), vec2(1, 0), vec2(0, 1),
        vec2(0, 1), vec2(1, 0), vec2(1, 1)
    );

    frag_uv = uvs[corner];
    frag_color = p.color;
    frag_texture_index = p.texture_index;

    // Flipbook UV remapping
    uint fb = p.flipbook_params;
    uint fb_total = (fb >> 8) & 0xFFu;
    if (fb_total > 0u) {
        uint fb_cols = fb & 0xFFu;
        uint fb_fps  = (fb >> 16) & 0xFFu;
        uint fb_mode = (fb >> 24) & 0x3u;
        uint fb_rows = (fb_total + fb_cols - 1u) / fb_cols;

        float age = 1.0 - p.lifetime / p.max_lifetime;
        uint frame;

        if (fb_mode == 0u) {
            // Over lifetime
            frame = uint(clamp(age, 0.0, 0.999) * float(fb_total));
        } else {
            // FPS-based
            float fps_val = float(fb_fps) * 0.25;
            float elapsed = age * p.max_lifetime;
            uint raw = uint(elapsed * fps_val);
            if (fb_mode == 1u)
                frame = raw % fb_total;  // loop
            else
                frame = min(raw, fb_total - 1u);  // clamp
        }

        uint col = frame % fb_cols;
        uint row = frame / fb_cols;
        float u_size = 1.0 / float(fb_cols);
        float v_size = 1.0 / float(fb_rows);
        float u0 = float(col) * u_size;
        float v0 = float(row) * v_size;

        // Small inset to avoid sampling neighboring frames at edges
        float eps = 0.01;
        frag_uv = vec2(mix(u0 + eps, u0 + u_size - eps, frag_uv.x),
                        mix(v0 + v_size - eps, v0 + eps, frag_uv.y));
    }

    vec3 world_pos;

    if (p.stretch > 0.0 && length(p.velocity) > 0.001) {
        // Velocity-aligned streak billboard
        vec3 vel_dir = normalize(p.velocity);
        vec3 to_cam = normalize(ubo.camera_position.xyz - p.position);
        vec3 right = normalize(cross(vel_dir, to_cam));

        // Stretch along velocity, thin perpendicular
        float half_length = p.size * p.stretch;
        float half_width = p.size;

        vec2 offset = offsets[corner];
        world_pos = p.position
            + vel_dir * offset.y * half_length
            + right * offset.x * half_width;
    } else {
        // Standard camera-facing billboard
        vec3 camera_right = vec3(ubo.view[0][0], ubo.view[1][0], ubo.view[2][0]);
        vec3 camera_up    = vec3(ubo.view[0][1], ubo.view[1][1], ubo.view[2][1]);

        vec2 offset = offsets[corner] * p.size;

        // Apply 2D rotation in billboard plane
        if (p.rotation != 0.0) {
            float c = cos(p.rotation);
            float s = sin(p.rotation);
            offset = vec2(c * offset.x - s * offset.y,
                          s * offset.x + c * offset.y);
        }

        world_pos = p.position
            + camera_right * offset.x
            + camera_up * offset.y;
    }

    frag_world_pos = world_pos;
    frag_world_normal = normalize(ubo.camera_position.xyz - world_pos);
    frag_is_mesh = 0u;

    gl_Position = ubo.projection * ubo.view * vec4(world_pos, 1.0);
}
