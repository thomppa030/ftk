#pragma once

namespace fjell {

class PassBuilder;
struct DeclareContext;

/// Declares what a lit surface samples through the global descriptor set:
/// the shadow atlases, the sky cubemap, the forward+ tile lists, the
/// ambient occlusion and the contact shadows. Every pass whose draws run the surface lighting
/// (spatial, terrain, water, lit sprites, tilemaps and particles) declares
/// them, so the graph orders their producers first and keeps them alive.
void declare_lit_surface_reads(PassBuilder& builder, const DeclareContext& ctx);

} // namespace fjell
