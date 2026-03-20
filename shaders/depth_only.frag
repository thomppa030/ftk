#version 460

// Trivial fragment shader for depth-only passes.
// Required so the driver respects pMultisampleState (fragment output interface
// state is only included when a fragment stage is present).

void main() {
    // depth is written automatically; no color output needed
}
