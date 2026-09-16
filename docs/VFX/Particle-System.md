# VFX — Particle System

> Stub — fill in.

## Scope
Module-based GPU particle system. Authored in the VFX graph editor, runs as compute + indirect-instanced draw. Supports lit particles, mesh particles, sub-emitters, distortion, trails, curves.

## Planned sections
- Module model — each module mutates particle state, ordered execution
- Emission modules — rate, burst, location (point, box, sphere, mesh)
- Initialization modules — initial velocity, color, size, lifetime, rotation
- Update modules — gravity, drag, curl noise, attractors, collision (scene SDF)
- Render modules
  - Billboard sprite (default)
  - Lit sprites (PBR-shaded)
  - Mesh particles (instance any model)
  - Trail / ribbon
  - Distortion (post-process pass)
- Sub-emitters — spawn child systems on event (birth, death, collision)
- Culling — distance, screen size, frustum
- Curves — drive any module parameter with a curve over particle life
- Velocity inheritance from emitter
- Performance: ring buffer, indirect draw, opaque vs transparent buckets
- Where it lives: `src/vfx/`, `src/renderer/pass/particle_*`
