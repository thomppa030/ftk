# Viewports — Render-to-Texture

> Stub — fill in.

## Scope
`SceneViewport` renders the scene from any camera into a bindless texture that materials can sample. Used for security cameras, mirrors, portals, minimaps, in-world screens.

## Planned sections
- Script API (`viewport_api.hpp`)
  ```cpp
  viewport::create("security_cam", 512, 512, VP_TONEMAP | VP_BLOOM);
  viewport::set_camera("security_cam", camera_node_id);
  viewport::destroy("security_cam");
  ```
- Feature flags
  - `VP_TONEMAP`, `VP_BLOOM`, `VP_GTAO`, `VP_SSR`, `VP_VOLUMETRIC_FOG`, `VP_HIZ`, `VP_SHADOWS`
  - Presets: `VP_MINIMAL`, `VP_FULL`
- Material reference syntax — `"display": "$viewport:security_cam"` in `.fjmat`
- Sampling in FJSL — same as any `sampler2D`
- Internals — `OffscreenPass`, `PostProcessResources`, bindless `reserve_index()` / `update_entry()`
- Current limitations
  - V1 RT reflections not included in camera-preview viewports
  - Cost scales with feature flags — be careful with VP_FULL on many viewports
- Recipes: CCTV monitor, water reflection, portal
