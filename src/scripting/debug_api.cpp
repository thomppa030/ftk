// debug_api.cpp — C ABI bridge from game .so to DebugDrawQueue
#include "fjell/api.h"
#include "scripting/script_globals.hpp"
#include "scripting/debug_draw_queue.hpp"

extern "C" FJELL_SDK_EXPORT void fjell_debug_line(float x0, float y0, float z0,
                                                  float x1, float y1, float z1,
                                                  float r, float g, float b, float a,
                                                  float ttl) {
    auto* q = fjell::script_services().debug_queue;
    if (q) q->line({x0, y0, z0}, {x1, y1, z1}, {r, g, b, a}, ttl);
}

extern "C" FJELL_SDK_EXPORT void fjell_debug_box(float cx, float cy, float cz,
                                                 float hx, float hy, float hz,
                                                 float r, float g, float b, float a,
                                                 float ttl) {
    auto* q = fjell::script_services().debug_queue;
    if (q) q->box({cx, cy, cz}, {hx, hy, hz}, {r, g, b, a}, ttl);
}

extern "C" FJELL_SDK_EXPORT void fjell_debug_sphere(float cx, float cy, float cz,
                                                    float radius,
                                                    float r, float g, float b, float a,
                                                    float ttl) {
    auto* q = fjell::script_services().debug_queue;
    if (q) q->sphere({cx, cy, cz}, radius, {r, g, b, a}, ttl);
}

extern "C" FJELL_SDK_EXPORT void fjell_debug_point(float x, float y, float z,
                                                   float r, float g, float b, float a,
                                                   float ttl) {
    auto* q = fjell::script_services().debug_queue;
    if (q) q->point({x, y, z}, {r, g, b, a}, ttl);
}
