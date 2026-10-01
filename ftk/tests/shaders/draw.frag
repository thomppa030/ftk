#version 460

// Fills what draw.vert or draw.mesh covers with the pushed colour, for the GPU
// library's link test (tests/library_link/gpu.cpp).

layout(push_constant) uniform Push { vec4 colour; } push;

layout(location = 0) out vec4 out_colour;

void main() {
    out_colour = push.colour;
}
