#pragma once

namespace ftk::gpu {
class RenderEncoder;
} // namespace ftk::gpu

namespace fjell {

struct FrameContext;

/// Draws the render queue's static meshes, opaque then transparent, with the
/// pipeline set on `pass`, pushing each one's world matrix: what the mesh
/// visualisations (UV, normals, wireframe, overdraw) draw. Skinned meshes are
/// left out; the viz pipelines have no skinned variant.
void draw_viz_meshes(const FrameContext& ctx, ftk::gpu::RenderEncoder& pass);

} // namespace fjell
