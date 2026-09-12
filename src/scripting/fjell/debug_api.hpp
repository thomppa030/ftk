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

/// Draws debug geometry and text. Everything here is a visualization aid —
/// none of it renders in a shipping build's normal output, and all of it is
/// safe to call every frame from `tick()`.
///
/// @code
/// void tick(float dt) override {
///     fjell::Debug::ray(transform().position, aim_dir_ * 5.0f);
///     fjell::Debug::watch_f("speed", "%.1f", glm::length(velocity_));
/// }
/// @endcode
struct Debug {
    /// Draw a line segment between two world positions.
    /// @param from     World-space start point.
    /// @param to       World-space end point.
    /// @param color    RGBA, each component 0-1. Defaults to green.
    /// @param duration Seconds to keep drawing it. 0 means this frame only,
    ///                 which is what you want when calling every frame.
    static void line(const glm::vec3& from, const glm::vec3& to,
                     const glm::vec4& color = {0.0f, 1.0f, 0.0f, 1.0f},
                     float duration = 0.0f) {
        fjell_debug_line(from.x, from.y, from.z, to.x, to.y, to.z,
                         color.r, color.g, color.b, color.a, duration);
    }

    /// Draw a wireframe box, axis-aligned in world space.
    /// @param center       World-space center of the box.
    /// @param half_extents Half the size on each axis — a 2x2x2 box is `{1,1,1}`.
    /// @param color        RGBA, each component 0-1. Defaults to green.
    /// @param duration     Seconds to keep drawing it. 0 means this frame only.
    static void box(const glm::vec3& center, const glm::vec3& half_extents,
                    const glm::vec4& color = {0.0f, 1.0f, 0.0f, 1.0f},
                    float duration = 0.0f) {
        fjell_debug_box(center.x, center.y, center.z,
                        half_extents.x, half_extents.y, half_extents.z,
                        color.r, color.g, color.b, color.a, duration);
    }

    /// Draw a wireframe sphere.
    /// @param center   World-space center.
    /// @param radius   Radius in world units.
    /// @param color    RGBA, each component 0-1. Defaults to green.
    /// @param duration Seconds to keep drawing it. 0 means this frame only.
    static void sphere(const glm::vec3& center, float radius,
                       const glm::vec4& color = {0.0f, 1.0f, 0.0f, 1.0f},
                       float duration = 0.0f) {
        fjell_debug_sphere(center.x, center.y, center.z, radius,
                           color.r, color.g, color.b, color.a, duration);
    }

    /// Mark a single world position with a small cross, sized in world units
    /// so it shrinks with distance.
    /// @param pos      World-space position to mark.
    /// @param color    RGBA, each component 0-1. Defaults to yellow.
    /// @param duration Seconds to keep drawing it. 0 means this frame only.
    static void point(const glm::vec3& pos,
                      const glm::vec4& color = {1.0f, 1.0f, 0.0f, 1.0f},
                      float duration = 0.0f) {
        fjell_debug_point(pos.x, pos.y, pos.z,
                          color.r, color.g, color.b, color.a, duration);
    }

    /// Draw a ray as a line from an origin along a direction.
    /// @param origin    World-space start point.
    /// @param direction Direction AND length — the ray ends at
    ///                  `origin + direction`, so scale it to the length you
    ///                  want rather than passing a unit vector.
    /// @param color     RGBA, each component 0-1. Defaults to green.
    /// @param duration  Seconds to keep drawing it. 0 means this frame only.
    static void ray(const glm::vec3& origin, const glm::vec3& direction,
                    const glm::vec4& color = {0.0f, 1.0f, 0.0f, 1.0f},
                    float duration = 0.0f) {
        line(origin, origin + direction, color, duration);
    }

    // ── On-screen text ──────────────────────────────────────────────────

    /// Maximum length of one formatted debug line, including the terminator.
    /// Longer text is truncated rather than overflowing, and formatting never
    /// allocates.
    static constexpr int kTextCapacity = 512;

    /// Show a keyed line of text on screen. Re-using a key replaces that line
    /// in place, so calling this every frame from `tick()` gives one steady
    /// readout instead of a scrolling flood.
    /// @param key      Your own identifier for this line — any int you pick.
    ///                 Lines with different keys stack; -1 means unkeyed, same
    ///                 as `print()`.
    /// @param text     The text to show.
    /// @param color    RGBA, each component 0-1. Defaults to white.
    /// @param duration Seconds to keep it on screen. 0 means this frame only.
    static void screen(int key, const char* text,
                       const glm::vec4& color = {1.0f, 1.0f, 1.0f, 1.0f},
                       float duration = 0.0f) {
        fjell_debug_screen(key, text, color.r, color.g, color.b, color.a, duration);
    }

    /// `printf`-style version of `screen()`. Colour and duration come before
    /// the format string because C varargs must come last.
    /// @param key      Your own identifier for this line; -1 means unkeyed.
    /// @param color    RGBA, each component 0-1.
    /// @param duration Seconds to keep it on screen. 0 means this frame only.
    /// @param fmt      `printf` format string; the compiler type-checks the
    ///                 arguments against it. Output longer than
    ///                 `kTextCapacity` is truncated.
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

    /// Append a new line of on-screen text, like a console print.
    /// @param text     The text to show.
    /// @param color    RGBA, each component 0-1. Defaults to white.
    /// @param duration Seconds to keep it on screen; defaults to 2.
    /// @note Every call adds another line. For something logged each frame use
    ///       `screen()` or `watch()`, which update in place instead of flooding.
    static void print(const char* text,
                      const glm::vec4& color = {1.0f, 1.0f, 1.0f, 1.0f},
                      float duration = 2.0f) {
        fjell_debug_screen(-1, text, color.r, color.g, color.b, color.a, duration);
    }

    /// `printf`-style version of `print()`.
    /// @param color    RGBA, each component 0-1.
    /// @param duration Seconds to keep it on screen.
    /// @param fmt      `printf` format string; type-checked against the
    ///                 arguments. Output is truncated at `kTextCapacity`.
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

    /// Show a named value as a `name: value` row — the readout to reach for
    /// when watching something change every frame.
    ///
    /// Rows live in their own block, sorted by name, so they hold still while
    /// messages come and go. Re-submitting a name updates it in place, and a
    /// row disappears the frame after you stop writing it.
    /// @param name  Row label; also the identity, so keep it stable.
    /// @param value Pre-formatted value. Use `watch_f()` to format inline.
    /// @param color RGBA, each component 0-1. Defaults to white.
    static void watch(const char* name, const char* value,
                      const glm::vec4& color = {1.0f, 1.0f, 1.0f, 1.0f}) {
        fjell_debug_watch(name, value, color.r, color.g, color.b, color.a);
    }

    /// `printf`-style version of `watch()` — the common case.
    /// @param name Row label; also the identity, so keep it stable.
    /// @param fmt  `printf` format string; type-checked against the arguments.
    /// @code
    /// fjell::Debug::watch_f("hp", "%d/%d", hp, max_hp);
    /// @endcode
    FJELL_PRINTF_FMT(2, 3)
    static void watch_f(const char* name, const char* fmt, ...) {
        char buf[kTextCapacity];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fjell_debug_watch(name, buf, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    /// `printf`-style watch row in a chosen colour — useful for turning a row
    /// red as a value crosses a threshold.
    /// @param name  Row label; also the identity, so keep it stable.
    /// @param color RGBA, each component 0-1.
    /// @param fmt   `printf` format string; type-checked against the arguments.
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

    /// Draw text anchored to a world position, projected to the screen each
    /// frame — for labelling objects in the scene.
    /// @param world_pos World-space anchor point.
    /// @param text      The text to show.
    /// @param color     RGBA, each component 0-1. Defaults to white.
    /// @param duration  Seconds to keep it. 0 means this frame only.
    /// @note Drawn at a fixed size regardless of distance, and hidden when the
    ///       anchor is behind the camera or off-screen.
    static void text_3d(const glm::vec3& world_pos, const char* text,
                        const glm::vec4& color = {1.0f, 1.0f, 1.0f, 1.0f},
                        float duration = 0.0f) {
        fjell_debug_text_3d(world_pos.x, world_pos.y, world_pos.z, text,
                            color.r, color.g, color.b, color.a, duration);
    }

    /// `printf`-style version of `text_3d()`.
    /// @param world_pos World-space anchor point.
    /// @param color     RGBA, each component 0-1.
    /// @param duration  Seconds to keep it. 0 means this frame only.
    /// @param fmt       `printf` format string; type-checked against the
    ///                  arguments. Truncated at `kTextCapacity`.
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
