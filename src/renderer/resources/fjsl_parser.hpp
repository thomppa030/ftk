#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

struct NamedValue;

/// How many of a material's texture slots are the fixed PBR set, and how many
/// are a shader's own. Named here, where the parser assigns them, rather than
/// spelled as literals: the split has to agree across GpuMaterial in
/// bindless.hpp, its hand-written GLSL mirror in fjell_gpu_types.glsl,
/// Material's own array and its bounds check, and this file's cap.
///
/// Twelve custom rather than four: a composed shader shares them with every
/// shader_function inlined into it, and four is one texture each for a sky and
/// three contributions.
constexpr uint32_t PBR_TEXTURE_SLOTS = 4;
constexpr uint32_t CUSTOM_TEXTURE_SLOTS = 12;
constexpr uint32_t TOTAL_TEXTURE_SLOTS = PBR_TEXTURE_SLOTS + CUSTOM_TEXTURE_SLOTS;

/// What a shader is: the pipeline shape it is drawn with, the template its
/// code is generated into, and the built-ins it has in scope. Engine-provided
/// and closed — a .fjsl names one of these or it does not parse.
///
/// The seven are four families. Spatial, terrain and water are lit surfaces:
/// same lighting code, same G-buffer, a light() hook. Sky and ui emit a colour
/// and are lit by nothing. Fog is a participating medium: no surface at all,
/// a density and a colour at a world point, lit per light through its own
/// light() with a phase angle where a surface has a normal. Post is what the
/// finished frame looks like: the exposed scene colour in, the display colour
/// out. What separates the members of a family is how their geometry arrives,
/// which is why each needs a pipeline of its own.
enum class ShaderType : uint8_t {
    Spatial, ///< a mesh, drawn from the vertex or mesh shader path
    Sky,     ///< one fullscreen triangle, also baked to the lighting cubemap
    Ui,      ///< a screen-space or worldspace quad
    Terrain, ///< generated from a .fjterrain height texture in a mesh shader
    Water,   ///< generated from a .fjwater wave field in a mesh shader
    Fog,     ///< a medium sampled per froxel by the volumetric fog compute pass
    Post,    ///< the scene colour to the display colour, one fullscreen triangle
};

/// The spelling a .fjsl uses for `type`, for diagnostics and asset files.
[[nodiscard]] std::string_view shader_type_name(ShaderType type);

/// The ShaderType a .fjsl's `shader_type` keyword names, or nullopt when the
/// word is not one of them. The parser reports that as an error; callers
/// reading a stored string (a .fjmat, a test) decide for themselves.
[[nodiscard]] std::optional<ShaderType> shader_type_from_name(std::string_view name);

/// What a .fjsl declares itself to be, read the way parse_fjsl() reads it
/// but without parsing the rest: the Type after `shader_type`, or nullopt
/// for a `shader_function;` library, a missing declaration or a word that
/// names no Type. For deciding which slot a shader or material can go in
/// without compiling it.
[[nodiscard]] std::optional<ShaderType> declared_shader_type(const std::string& source);

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
    /// hint_enum("A,B,C") on an int: the value is an index into
    /// FjslUniform::enum_options and the inspector shows a drop-down.
    Enum,
    /// hint_lut on a sampler2D: a colour grading table from a `.fjlut`,
    /// loaded 16-bit and clamped, and the neutral table when none is set.
    Lut,
};

struct HintRange {
    float min{0.0f};
    float max{1.0f};
    float step{0.01f};
    /// True when hint_range gave these. A float shows a 0..1 slider either
    /// way; an int shows a slider only between ends the author chose.
    bool declared{false};
};

// Uniform type as declared in .fjsl
enum class UniformType {
    Float,
    /// A whole number. It takes one float of the params like a Float, so a
    /// track or a blend moves it as a number, and the generated GLSL rounds
    /// it, so the shader sees a whole one.
    Int,
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
    /// hint_enum's names, in index order. Empty for any other hint.
    std::vector<std::string> enum_options;
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
    /// Which file this is about, when it is not the one being parsed. A
    /// composition reports a used function's problems against that function,
    /// with its own line numbers; empty means the file under the cursor.
    std::string file;
};

/// One `in` or `out` of a shader_function: a value the caller passes in, or one
/// the function writes for the caller to read.
struct FjslPort {
    std::string name;
    UniformType type{UniformType::Float};
    /// Byte offset of the declaration, for diagnostics about this port.
    size_t offset{0};
};

/// One `use name = "path.fjsl"(arg: EXPR, ...)`: a function wired into this
/// file, under a name the consumer chose.
struct FjslUse {
    std::string name;                 ///< the consumer's name for this wiring
    std::string path;                 ///< as written, resolved by the caller
    /// Arguments in source order, `in`-name to the expression filling it. Kept
    /// as text: they are consumer-side GLSL and are substituted verbatim.
    std::vector<std::pair<std::string, std::string>> arguments;
    size_t offset{0};
};

// Complete metadata extracted from a .fjsl file
struct ShaderMetadata {
    /// True when the file declared `shader_function;` rather than a
    /// `shader_type`: a pure function, drawn by no pass, composed into others.
    /// `shader_type` is meaningless when this is set.
    bool is_function{false};
    ShaderType shader_type{ShaderType::Spatial};
    RenderMode render_mode{RenderMode::Lit};
    RenderState render_state;
    std::vector<FjslUniform> uniforms;
    /// shader_function only: what main() is passed and what it writes.
    std::vector<FjslPort> inputs;
    std::vector<FjslPort> outputs;
    /// Functions this file composes, in source order.
    std::vector<FjslUse> uses;
    FjslBody fragment_body;    // contents of fragment() { ... }; sky() for a sky, density() for fog, color() for post
    FjslBody vertex_body;      // contents of vertex() { ... } (empty = standard)
    FjslBody light_body;       // contents of light() { ... } (empty = the type's default: PBR, or the fog phase)
    FjslBody layers_body;      // terrain only: contents of layers() { ... } (empty = painted weights as-is)
    FjslBody main_body;        // shader_function only: contents of main() { ... }
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

    /// The Type this file is, or nullopt when it is a shader_function. This is
    /// what decides which hooks it may define and which template it generates
    /// into; read it rather than `shader_type` wherever a function is possible.
    [[nodiscard]] std::optional<ShaderType> type() const {
        return is_function ? std::nullopt : std::optional{shader_type};
    }

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

/// One file of a composition: what a `use` resolved to, parsed.
struct FjslComposedFile {
    std::string path;      ///< as the `use` wrote it
    std::string use_name;  ///< the consumer's local name for it; empty for the consumer itself
    ShaderMetadata meta;
};

/// A composition: the consumer, then every function reachable from its `use`
/// declarations, each appearing once no matter how many files wire it.
///
/// Ordered so a file comes after everything it uses, which is the order the
/// generator emits them in — a function's helpers must be declared before the
/// code that calls them.
struct FjslComposition {
    std::vector<FjslComposedFile> files;  ///< dependencies first, consumer last
    std::vector<FjslDiagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                           [](const FjslDiagnostic& d) {
                               return d.severity == FjslSeverity::error;
                           });
    }
};

/// Source of a `use`d file, by the path the `use` wrote. Returns nullopt when
/// nothing is there; the resolver reports that against the declaration.
using FjslSourceReader =
    std::function<std::optional<std::string>(const std::string& path)>;

/// Resolve a parsed file's `use` declarations, recursively, and check that the
/// wiring is sound: every path exists and is a shader_function, every `in` is
/// filled exactly once, no file uses itself directly or through another, and
/// the chain is not absurdly deep.
///
/// `meta` is the consumer, already parsed. Diagnostics from a used file are
/// reported against that file, with its own line numbers, so an error names
/// where it lives rather than where it was wired.
[[nodiscard]] FjslComposition resolve_composition(const ShaderMetadata& meta,
                                                  const std::string& path,
                                                  const FjslSourceReader& read);

/// Write a parse's diagnostics to the log as
/// `file:line:column: severity: message` — the compiler convention, which
/// editors and their linter plugins parse without being configured for it.
/// `name` is what the file should be called; empty reads as `<fjsl>`.
void log_fjsl_diagnostics(const ShaderMetadata& meta, const std::string& name);

/// The same, for a composition: each diagnostic names the file it belongs to,
/// which for a used function is that function rather than `name`.
void log_fjsl_diagnostics(const struct FjslComposition& composition,
                          const std::string& name);

/// How generated GLSL reaches a program's uniforms: `param(i)` is the
/// expression reading float `i` of its parameter block, `texture(slot)` the
/// sampler in texture slot `slot`, numbered as FjslUniform::slot numbers
/// samplers: from PBR_TEXTURE_SLOTS, after a material's own four. Each shader
/// type's template fixes these for the engine's materials; a host with a
/// template of its own says where its values live.
struct FjslUniformAccess {
    std::function<std::string(uint32_t index)> param;
    std::function<std::string(uint32_t slot)> texture;
};

/// The functions `composition` wires in, as GLSL for file scope, dependencies
/// first: each composed file's helpers and constants, its uniforms as
/// #defines read through `access`, its outputs as variables, and its main()
/// as a function named for its use (`sun_main`), every name it declares
/// prefixed the same way. Their uniforms take runs of the parameter block and
/// of the texture slots after `param_base` and `texture_base`, the consumer's
/// own counts, in file order.
[[nodiscard]] std::string emit_composition(const FjslComposition& composition,
                                           const FjslUniformAccess& access,
                                           uint32_t param_base, uint32_t texture_base);

/// The calls that run each function a consumer uses, in the order its `use`s
/// are written, passing what each wired in. They go ahead of the body that
/// reads what the functions wrote.
[[nodiscard]] std::string emit_use_calls(const std::vector<FjslUse>& uses,
                                         const FjslComposition& composition);

/// `body` with its reads of used functions (`sun.color`) turned into the names
/// emit_composition() declares (`sun_color`). Only a name a `use` introduced
/// is touched, so an ordinary `v.xyz` is left alone.
[[nodiscard]] std::string rewrite_use_references(const std::string& body,
                                                 const std::vector<FjslUse>& uses);

// Generate a complete .frag GLSL file from the spatial template + FJSL metadata.
//
// `composition`, when given, is what resolve_composition returned for this
// file: every function it uses is emitted at file scope ahead of the user's own
// globals, with its identifiers mangled under the name the `use` gave it.
[[nodiscard]] std::string generate_fragment_shader(
    const ShaderMetadata& meta, const std::string& template_source,
    const struct FjslComposition* composition = nullptr);

// Generate a .vert or .mesh GLSL file from a vertex/mesh template + FJSL metadata.
// Used for both vertex shaders and mesh shaders — same placeholder substitution.
// The uniforms are read through the same functions as on the fragment side
// and the file's helpers that vertex() reaches go in with them, so a helper
// reads what the hook reads.
// Each stage gets only the helpers its own hooks reach.
[[nodiscard]] std::string generate_vertex_shader(const ShaderMetadata& meta,
                                                  const std::string& template_source);

// Return the number of floats consumed by a uniform type.
[[nodiscard]] uint32_t uniform_float_count(UniformType type);

/// The GLSL spelling of a uniform/port type.
[[nodiscard]] std::string_view glsl_type_name(UniformType type);

/// Turn an authored uniform value into what the params SSBO holds. A
/// `hint_color` is authored in sRGB everywhere a person sees it — the `.fjsl`
/// default, the `.fjmat`, the colour picker, the script API — while the GPU
/// multiplies linear light, so its RGB is converted here. Alpha and every
/// other uniform pass through untouched.
void encode_uniform_for_gpu(const FjslUniform& u, std::span<float> values);

/// Inverse of encode_uniform_for_gpu, for a value read back from the SSBO
/// that is about to be saved to a `.fjmat` or shown in a colour picker.
void decode_uniform_from_gpu(const FjslUniform& u, std::span<float> values);

/// The block of floats a shader's scalar uniforms read, as the GPU holds
/// them: each uniform's .fjsl default, the value of the same name in
/// `values` over it, encoded. A value shorter than its uniform keeps the
/// default in the components it leaves out. `meta.custom_param_count` long.
[[nodiscard]] std::vector<float> resolve_uniform_block(const ShaderMetadata& meta,
                                                       const std::vector<NamedValue>& values);

} // namespace fjell
