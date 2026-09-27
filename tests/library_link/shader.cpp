// Links fjell-shader alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its own
// dependencies. Running it parses a shader, generates its GLSL and reads a
// compiler error back, none of which needs a GPU or glslc.

#include "core/log.hpp"
#include "renderer/resources/fjsl_parser.hpp"
#include "renderer/shader_diagnostic.hpp"

#include <string>

int main() {
    fjell::log::init({.level = spdlog::level::warn});

    const fjell::ShaderMetadata meta = fjell::parse_fjsl(
        "shader_type spatial;\n"
        "uniform float strength : hint_range(0.0, 1.0, 0.01) = 0.5;\n"
        "void fragment() { ALBEDO *= strength; }\n");
    const std::string glsl = fjell::generate_fragment_shader(meta, "#version 460\n");
    const fjell::ShaderDiagnostic error =
        fjell::first_shader_error("tint.fjsl:3: error: 'strenght' : undeclared identifier");

    fjell::log::shutdown();
    const bool ran = meta.uniforms.size() == 1 && !glsl.empty() && error.line == 3;
    return ran ? 0 : 1;
}
