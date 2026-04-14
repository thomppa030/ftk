// ── Global descriptor set (set 0) binding assignments ──────────────────
//
// Bindings 0-10 are declared in fjell_spatial_template.glsl and individual
// shader files. New bindings (11+) go here.
//
// C++ side: GlobalBindings in renderer/global_bindings.hpp manages the
// layout, pool sizing, and descriptor writes. Each binding has a string
// name so writes use names instead of magic numbers.
//
// Current bindings:
//   0  global_ubo         UNIFORM_BUFFER
//   1  shadow_map          COMBINED_IMAGE_SAMPLER
//   2  light_ssbo          STORAGE_BUFFER
//   3  tile_light_ssbo     STORAGE_BUFFER
//   4  depth_texture       COMBINED_IMAGE_SAMPLER
//   5  ao_texture          COMBINED_IMAGE_SAMPLER
//   6  sky_cubemap         COMBINED_IMAGE_SAMPLER
//   7  brdf_lut            COMBINED_IMAGE_SAMPLER
//   8  screen_color        COMBINED_IMAGE_SAMPLER
//   9  scene_sdf           COMBINED_IMAGE_SAMPLER
//   10 gi_indirect         COMBINED_IMAGE_SAMPLER
//   11 local_shadow_atlas  COMBINED_IMAGE_SAMPLER
//   12 ss_gi_emissive      COMBINED_IMAGE_SAMPLER
//
// To add a new binding:
//   1. Call gpu_.global_bindings().declare({...}) in C++ before finalize
//   2. Add the GLSL declaration below with the assigned binding number
//   3. Include this file in shaders that need the new binding
