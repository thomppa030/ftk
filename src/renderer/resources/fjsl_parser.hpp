#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fjell {

// Lighting mode parsed from .fjsl files
enum class RenderMode : uint8_t {
    Lit,    // default PBR lighting
    Unlit,  // skip lighting, output ALBEDO + EMISSION directly
};

// Blend mode for materials
enum class BlendMode : uint8_t {
    Alpha,     // default alpha blending for transparent
    Add,       // additive (particles, holograms)
    Multiply,  // multiplicative
};

// Per-material render state parsed from render_mode keywords
struct RenderState {
    bool cull_disabled{false};
    bool cull_front{false};        // front-face culling (e.g. outline pass)
    bool depth_write_disabled{false};
    bool depth_test_disabled{false};
    bool force_transparent{false}; // explicit blend_alpha/blend_add/blend_multiply
    BlendMode blend{BlendMode::Alpha};
};

// Uniform hint types parsed from .fjsl files
enum class UniformHint {
    None,
    Color,
    White,
    Black,
    Normal,
    DefaultMr,
};

struct HintRange {
    float min{0.0f};
    float max{1.0f};
    float step{0.01f};
};

// Uniform type as declared in .fjsl
enum class UniformType {
    Float,
    Vec2,
    Vec3,
    Vec4,
    Sampler2D,
};

// A parsed uniform declaration from .fjsl
struct FjslUniform {
    std::string name;
    UniformType type{UniformType::Float};
    UniformHint hint{UniformHint::None};
    HintRange range{};
    // hint_emission: this uniform is part of what the surface emits. The
    // product of all such uniforms is the material's emission for the GI
    // paths, which cannot run fragment() and need a value they can read.
    // Separate from `hint` because it combines with it: an emission colour
    // still wants a colour picker, an emission strength still wants a slider.
    bool emission{false};
    // hint_albedo: this uniform is part of the surface colour. The product
    // of all such uniforms multiplies the base colour texture before
    // fragment() runs, and is what the GI paths read as the material's
    // albedo, so a shader that tags its tint does not apply it again.
    bool albedo{false};
    /// The heading this uniform was declared under, from the last `group`
    /// before it, or empty for the ones above the first. Authoring only: the
    /// inspector sections its sliders by this so a shader with forty dials
    /// reads as a handful of subjects rather than one wall.
    std::string group;
    std::vector<float> default_value; // scalar or vector components
    uint32_t slot{0}; // texture slot (4-7) for sampler2D, or param offset for scalars
};

/// One hook's body and where it began in the .fjsl, so the generator can emit
/// a `#line` ahead of it and a compiler error lands on the line the author
/// wrote instead of a line in the generated file. `offset` is a byte offset
/// into the source `parse_fjsl` was given; it means nothing when the body is
/// empty.
struct FjslBody {
    std::string code;
    size_t offset{0};

    [[nodiscard]] bool empty() const { return code.empty(); }
};

enum class FjslSeverity : uint8_t {
    warning, ///< the shader still compiles; something was ignored or guessed
    error,   ///< the shader cannot be built from this
};

/// A problem found while parsing, positioned so an editor can point at the
/// offending text and a log line can name file:line:column.
///
/// Diagnostics are collected rather than thrown so one bad line does not hide
/// the rest of the file — a linter wants every problem in one pass, and a
/// shader with two typos should report two.
struct FjslDiagnostic {
    uint32_t line{1};   ///< 1-based
    uint32_t column{1}; ///< 1-based, in bytes
    FjslSeverity severity{FjslSeverity::warning};
    std::string message;
};

// Complete metadata extracted from a .fjsl file
struct ShaderMetadata {
    std::string shader_type; // "spatial", "sky", "ui" or "terrain"
    RenderMode render_mode{RenderMode::Lit};
    RenderState render_state;
    std::vector<FjslUniform> uniforms;
    FjslBody fragment_body;    // contents of fragment() { ... }
    FjslBody vertex_body;      // contents of vertex() { ... } (empty = standard)
    FjslBody light_body;       // contents of light() { ... } (empty = default PBR)
    FjslBody layers_body;      // terrain only: contents of layers() { ... } (empty = painted weights as-is)
    std::string user_globals;  // top-level helpers / constants the user authored
    /// What the generated GLSL should call this file in `#line` directives.
    /// Set by the caller that knows the path; empty falls back to numeric-only
    /// `#line`, which still fixes the line number.
    std::string source_path;
    /// The source this was parsed from, kept so a body's byte offset can be
    /// turned into a line number when the GLSL is generated. Parsing and
    /// generating are separate calls, and the offsets mean nothing without it.
    std::string source_text;
    // render_mode no_weather: the surface takes no rain or snow from the
    // environment; fragment() reads the weather built-ins and decides itself.
    bool no_weather{false};
    uint32_t custom_texture_count{0};
    uint32_t custom_param_count{0}; // total floats needed for scalar uniforms
    /// Everything the parse found wrong, in source order. Warnings mean the
    /// shader still builds; an error means the metadata is not usable. Empty
    /// on a clean parse.
    std::vector<FjslDiagnostic> diagnostics;

    /// True when any diagnostic is an error.
    [[nodiscard]] bool has_errors() const {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                           [](const FjslDiagnostic& d) {
                               return d.severity == FjslSeverity::error;
                           });
    }
};

// Parse a .fjsl file and extract metadata.
[[nodiscard]] ShaderMetadata parse_fjsl(const std::string& source);

/// Write a parse's diagnostics to the log as
/// `file:line:column: severity: message` — the compiler convention, which
/// editors and their linter plugins parse without being configured for it.
/// `name` is what the file should be called; empty reads as `<fjsl>`.
void log_fjsl_diagnostics(const ShaderMetadata& meta, const std::string& name);

// Generate a complete .frag GLSL file from the spatial template + FJSL metadata.
[[nodiscard]] std::string generate_fragment_shader(const ShaderMetadata& meta,
                                                    const std::string& template_source);

// Generate a .vert or .mesh GLSL file from a vertex/mesh template + FJSL metadata.
// Used for both vertex shaders and mesh shaders — same placeholder substitution.
[[nodiscard]] std::string generate_vertex_shader(const ShaderMetadata& meta,
                                                  const std::string& template_source);

// Return the number of floats consumed by a uniform type.
[[nodiscard]] uint32_t uniform_float_count(UniformType type);

/// Turn an authored uniform value into what the params SSBO holds. A
/// `hint_color` is authored in sRGB everywhere a person sees it — the `.fjsl`
/// default, the `.fjmat`, the colour picker, the script API — while the GPU
/// multiplies linear light, so its RGB is converted here. Alpha and every
/// other uniform pass through untouched.
void encode_uniform_for_gpu(const FjslUniform& u, std::span<float> values);

/// Inverse of encode_uniform_for_gpu, for a value read back from the SSBO
/// that is about to be saved to a `.fjmat` or shown in a colour picker.
void decode_uniform_from_gpu(const FjslUniform& u, std::span<float> values);

} // namespace fjell
