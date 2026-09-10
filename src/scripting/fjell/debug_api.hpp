// debug_api.hpp — Static debug draw API for game scripts
#pragma once

#include "fjell/api.h"

#include <glm/glm.hpp>

#include <cstdarg>
#include <cstdio>

extern "C" {
FJELL_SDK_EXPORT void fjell_debug_line(float x0, float y0, float z0,
                                       float x1, float y1, float z1,
                                       float r, float g, float b, float a,
                                       float ttl);
FJELL_SDK_EXPORT void fjell_debug_box(float cx, float cy, float cz,
                                      float hx, float hy, float hz,
                                      float r, float g, float b, float a,
                                      float ttl);
FJELL_SDK_EXPORT void fjell_debug_sphere(float cx, float cy, float cz,
                                         float radius,
                                         float r, float g, float b, float a,
                                         float ttl);
FJELL_SDK_EXPORT void fjell_debug_point(float x, float y, float z,
                                        float r, float g, float b, float a,
                                        float ttl);

// Screen-space debug text. Formatting happens on the caller's side, so only
// a finished string crosses the ABI — no varargs on the boundary.
FJELL_SDK_EXPORT void fjell_debug_screen(int key, const char* text,
                                         float r, float g, float b, float a,
                                         float ttl);
FJELL_SDK_EXPORT void fjell_debug_watch(const char* name, const char* value,
                                        float r, float g, float b, float a);
FJELL_SDK_EXPORT void fjell_debug_text_3d(float x, float y, float z,
                                          const char* text,
                                          float r, float g, float b, float a,
                                          float ttl);
}

namespace fjell {

struct Debug {
    /// Draw a line from `from` to `to`. Duration 0 = single frame.
    static void line(const glm::vec3& from, const glm::vec3& to,
                     const glm::vec4& color = {0.0f, 1.0f, 0.0f, 1.0f},
                     float duration = 0.0f) {
        fjell_debug_line(from.x, from.y, from.z, to.x, to.y, to.z,
                         color.r, color.g, color.b, color.a, duration);
    }

    /// Draw a wireframe box centered at `center` with `half_extents`.
    static void box(const glm::vec3& center, const glm::vec3& half_extents,
                    const glm::vec4& color = {0.0f, 1.0f, 0.0f, 1.0f},
                    float duration = 0.0f) {
        fjell_debug_box(center.x, center.y, center.z,
                        half_extents.x, half_extents.y, half_extents.z,
                        color.r, color.g, color.b, color.a, duration);
    }

    /// Draw a wireframe sphere at `center` with `radius`.
    static void sphere(const glm::vec3& center, float radius,
                       const glm::vec4& color = {0.0f, 1.0f, 0.0f, 1.0f},
                       float duration = 0.0f) {
        fjell_debug_sphere(center.x, center.y, center.z, radius,
                           color.r, color.g, color.b, color.a, duration);
    }

    /// Draw a point (rendered as a small cross) at `pos`.
    static void point(const glm::vec3& pos,
                      const glm::vec4& color = {1.0f, 1.0f, 0.0f, 1.0f},
                      float duration = 0.0f) {
        fjell_debug_point(pos.x, pos.y, pos.z,
                          color.r, color.g, color.b, color.a, duration);
    }

    /// Draw a ray from `origin` in `direction` (length = magnitude of direction).
    static void ray(const glm::vec3& origin, const glm::vec3& direction,
                    const glm::vec4& color = {0.0f, 1.0f, 0.0f, 1.0f},
                    float duration = 0.0f) {
        line(origin, origin + direction, color, duration);
    }

    // ── On-screen text ──────────────────────────────────────────────────
    // Buffer for one formatted line. Larger than Log's because debug lines
    // routinely carry several values; vsnprintf truncates rather than
    // overflowing, and nothing here allocates.
    static constexpr int kTextCapacity = 512;

    /// Keyed message. Re-using a key replaces that line in place, so calling
    /// this every frame from tick() gives one steady readout instead of a
    /// scrolling flood. Duration 0 = single frame.
    static void screen(int key, const char* text,
                       const glm::vec4& color = {1.0f, 1.0f, 1.0f, 1.0f},
                       float duration = 0.0f) {
        fjell_debug_screen(key, text, color.r, color.g, color.b, color.a, duration);
    }

    /// Formatted keyed message. Colour and duration precede the format string
    /// because C varargs must come last.
    FJELL_PRINTF_FMT(4, 5)
    static void screen_f(int key, const glm::vec4& color, float duration,
                         const char* fmt, ...) {
        char buf[kTextCapacity];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fjell_debug_screen(key, buf, color.r, color.g, color.b, color.a, duration);
    }

    /// Unkeyed message — appends a new line each call. Use a positive
    /// `duration` so one-off events stay readable; per-frame calls belong in
    /// `screen`/`watch` instead.
    static void print(const char* text,
                      const glm::vec4& color = {1.0f, 1.0f, 1.0f, 1.0f},
                      float duration = 2.0f) {
        fjell_debug_screen(-1, text, color.r, color.g, color.b, color.a, duration);
    }

    /// Formatted unkeyed message.
    FJELL_PRINTF_FMT(3, 4)
    static void print_f(const glm::vec4& color, float duration,
                        const char* fmt, ...) {
        char buf[kTextCapacity];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fjell_debug_screen(-1, buf, color.r, color.g, color.b, color.a, duration);
    }

    /// Named value shown as a "name: value" row, sorted by name and held in
    /// its own block so rows don't shuffle as messages come and go. Identified
    /// by name, so re-submitting updates in place. Stops showing the frame
    /// after you stop writing it.
    static void watch(const char* name, const char* value,
                      const glm::vec4& color = {1.0f, 1.0f, 1.0f, 1.0f}) {
        fjell_debug_watch(name, value, color.r, color.g, color.b, color.a);
    }

    /// Formatted watch — the common case: `watch_f("hp", "%d/%d", hp, max)`.
    FJELL_PRINTF_FMT(2, 3)
    static void watch_f(const char* name, const char* fmt, ...) {
        char buf[kTextCapacity];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fjell_debug_watch(name, buf, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    /// Formatted watch with an explicit colour.
    FJELL_PRINTF_FMT(3, 4)
    static void watch_cf(const char* name, const glm::vec4& color,
                         const char* fmt, ...) {
        char buf[kTextCapacity];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fjell_debug_watch(name, buf, color.r, color.g, color.b, color.a);
    }

    /// Text anchored to a world position, projected to the screen each frame.
    /// Hidden when behind the camera or off-screen.
    static void text_3d(const glm::vec3& world_pos, const char* text,
                        const glm::vec4& color = {1.0f, 1.0f, 1.0f, 1.0f},
                        float duration = 0.0f) {
        fjell_debug_text_3d(world_pos.x, world_pos.y, world_pos.z, text,
                            color.r, color.g, color.b, color.a, duration);
    }

    /// Formatted world-anchored text.
    FJELL_PRINTF_FMT(4, 5)
    static void text_3d_f(const glm::vec3& world_pos, const glm::vec4& color,
                          float duration, const char* fmt, ...) {
        char buf[kTextCapacity];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fjell_debug_text_3d(world_pos.x, world_pos.y, world_pos.z, buf,
                            color.r, color.g, color.b, color.a, duration);
    }
};

} // namespace fjell
