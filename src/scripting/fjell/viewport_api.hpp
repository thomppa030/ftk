// viewport_api.hpp — Static viewport API for game scripts
#pragma once

#include "fjell/api.h"

#include <cstdint>

// Post-process feature flags for scene viewports.
enum FjellViewportFlags : uint32_t {
    VP_TONEMAP        = 1 << 0,
    VP_BLOOM          = 1 << 1,
    VP_GTAO           = 1 << 2,
    VP_SSR            = 1 << 3,
    VP_VOLUMETRIC_FOG = 1 << 4,
    VP_HIZ            = 1 << 5,
    VP_SHADOWS        = 1 << 6,  // re-render shadows (expensive, usually skip)
    VP_MINIMAL        = VP_TONEMAP,
    VP_FULL           = VP_TONEMAP | VP_BLOOM | VP_GTAO | VP_SSR | VP_VOLUMETRIC_FOG | VP_HIZ | VP_SHADOWS,
};

extern "C" {
FJELL_SDK_EXPORT uint32_t fjell_viewport_create(const char* name, uint32_t width, uint32_t height,
                                uint32_t flags);
FJELL_SDK_EXPORT void     fjell_viewport_destroy(const char* name);
FJELL_SDK_EXPORT void     fjell_viewport_set_camera(const char* name, uint32_t camera_node_id);
FJELL_SDK_EXPORT uint32_t fjell_viewport_bindless_index(const char* name);
FJELL_SDK_EXPORT void     fjell_viewport_resize(const char* name, uint32_t width, uint32_t height);
}

namespace fjell {

struct ViewportAPI {
    /// Create a scene viewport that renders to a texture.
    /// Returns the bindless texture index (usable in $viewport: materials).
    static uint32_t create(const char* name, uint32_t width = 512, uint32_t height = 512,
                           uint32_t flags = VP_MINIMAL) {
        return fjell_viewport_create(name, width, height, flags);
    }

    /// Destroy a scene viewport.
    static void destroy(const char* name) {
        fjell_viewport_destroy(name);
    }

    /// Point the viewport at a CameraNode each frame.
    static void set_camera(const char* name, uint32_t camera_node_id) {
        fjell_viewport_set_camera(name, camera_node_id);
    }

    /// Get the bindless texture index for use in material lookups.
    static uint32_t bindless_index(const char* name) {
        return fjell_viewport_bindless_index(name);
    }

    /// Resize the viewport.
    static void resize(const char* name, uint32_t width, uint32_t height) {
        fjell_viewport_resize(name, width, height);
    }
};

} // namespace fjell
