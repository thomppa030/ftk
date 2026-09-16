#pragma once

#include <span>

namespace fjell {

class BindlessTextures;
class Material;
struct FjslUniform;
struct ShaderMetadata;

/// Draw the control for one uniform's value, picked by its type and hint:
/// a slider for a ranged float, a colour picker for hint_color, a drag
/// otherwise. `values` holds the value the way a person reads it (colours
/// in sRGB) and is written back on change. The caller owns the ImGui ID.
/// Returns true when the value changed this frame.
[[nodiscard]] bool draw_uniform_value(const FjslUniform& u, std::span<float> values);

// Draw ImGui controls for all custom shader params (non-sampler uniforms).
// Reads/writes param values through the bindless SSBO.
// Returns true if any value was modified this frame.
[[nodiscard]] bool draw_material_params(const ShaderMetadata& meta,
                                         BindlessTextures& bindless,
                                         Material& material);

} // namespace fjell
