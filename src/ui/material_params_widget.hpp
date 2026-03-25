#pragma once

namespace fjell {

class BindlessTextures;
class Material;
struct ShaderMetadata;

// Draw ImGui controls for all custom shader params (non-sampler uniforms).
// Reads/writes param values through the bindless SSBO.
// Returns true if any value was modified this frame.
[[nodiscard]] bool draw_material_params(const ShaderMetadata& meta,
                                         BindlessTextures& bindless,
                                         Material& material);

} // namespace fjell
