#include "renderer/resources/fjsl_parser.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace fjell;

static struct LogInit {
    LogInit() { if (!log::core()) log::init(); }
} s_log_init;

// The parser collects problems rather than throwing on the first, so one bad
// line does not hide the rest of the file and a linter can report everything
// in one pass. A rejection is therefore an error diagnostic, and it has to
// carry a position or an editor cannot point at it.
static void require_rejected(const std::string& src) {
    auto meta = parse_fjsl(src);
    INFO("source: " << src);
    REQUIRE(meta.has_errors());
    bool located = false;
    for (const auto& d : meta.diagnostics) {
        if (d.severity == FjslSeverity::error) {
            INFO("message: " << d.message);
            REQUIRE(d.line >= 1);
            REQUIRE_FALSE(d.message.empty());
            located = true;
        }
    }
    REQUIRE(located);
}

// --- shader_type ---

TEST_CASE("Parse shader_type spatial", "[fjsl]") {
    auto meta = parse_fjsl("shader_type spatial;\nvoid fragment() {}");
    REQUIRE(meta.shader_type == ShaderType::Spatial);
}

TEST_CASE("A group heads the uniforms declared under it", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float loose;
        group "Colour";
        uniform vec3 tint : hint_color;
        uniform float strength;
        group "Foam";
        uniform float foam_edge;
        void fragment() {}
    )");

    REQUIRE(meta.uniforms.size() == 4);
    // Anything above the first heading belongs to none.
    CHECK(meta.uniforms[0].name == "loose");
    CHECK(meta.uniforms[0].group.empty());
    CHECK(meta.uniforms[1].name == "tint");
    CHECK(meta.uniforms[1].group == "Colour");
    // A heading runs until the next one, not just over the uniform after it.
    CHECK(meta.uniforms[2].name == "strength");
    CHECK(meta.uniforms[2].group == "Colour");
    CHECK(meta.uniforms[3].name == "foam_edge");
    CHECK(meta.uniforms[3].group == "Foam");
}

TEST_CASE("Every group in a shader is picked up in order", "[fjsl]") {
    // Twenty-odd headings in a row, which is the case that matters: a heading
    // has to run until the next one rather than over the uniform after it.
    std::string src = "shader_type spatial;\n";
    for (int g = 0; g < 20; ++g) {
        src += "group \"G" + std::to_string(g) + "\";\n";
        for (int u = 0; u < 3; ++u) {
            src += "uniform float g" + std::to_string(g) + "_" + std::to_string(u) + ";\n";
        }
    }
    src += "void fragment() {}\n";

    auto meta = parse_fjsl(src);
    REQUIRE(meta.uniforms.size() == 60);
    for (int g = 0; g < 20; ++g) {
        for (int u = 0; u < 3; ++u) {
            CHECK(meta.uniforms[g * 3 + u].group == "G" + std::to_string(g));
        }
    }
}

TEST_CASE("A group never reaches the generated GLSL", "[fjsl]") {
    // It is an authoring heading; GLSL has never heard of it, and leaving one
    // in is a syntax error at the line it sits on.
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        group "Colour";
        uniform vec3 tint : hint_color;
        void fragment() { ALBEDO = tint; }
    )");
    const std::string tmpl =
        "/*FJSL_CUSTOM_TEXTURES*/\n/*FJSL_USER_GLOBALS*/\n"
        "void main() {\n/*FJSL_USER_CODE*/\n}\n";
    const std::string glsl = generate_fragment_shader(meta, tmpl);
    CHECK(glsl.find("group") == std::string::npos);
    CHECK(glsl.find("Colour") == std::string::npos);
}

TEST_CASE("Missing shader_type is a located error", "[fjsl]") {
    require_rejected("void fragment() {}");
}

TEST_CASE("Unsupported shader_type is a located error", "[fjsl]") {
    require_rejected("shader_type canvas;\nvoid fragment() {}");
}

TEST_CASE("A default that is not a number names the uniform", "[fjsl]") {
    // This used to reach the user as the word "stof" and nothing else: an
    // unguarded std::stof threw out of the whole parse.
    auto meta = parse_fjsl("shader_type spatial;\n"
                           "uniform float amount = abc;\n"
                           "void fragment() {}");
    REQUIRE_FALSE(meta.diagnostics.empty());
    const auto& d = meta.diagnostics.front();
    INFO("message: " << d.message);
    CHECK(d.message.find("amount") != std::string::npos);
    CHECK(d.message.find("abc") != std::string::npos);
    CHECK(d.line == 2);
    // A bad default is not fatal — the uniform still exists, at zero.
    CHECK_FALSE(meta.has_errors());
    REQUIRE(meta.uniforms.size() == 1);
}

TEST_CASE("A partly-numeric vector default is refused whole", "[fjsl]") {
    // Keeping the components that did parse would put half a colour on the
    // surface, which reads as a bug in the shader rather than in the file.
    auto meta = parse_fjsl("shader_type spatial;\n"
                           "uniform vec3 tint = vec3(1.0, oops, 0.5);\n"
                           "void fragment() {}");
    REQUIRE_FALSE(meta.diagnostics.empty());
    CHECK(meta.diagnostics.front().message.find("oops") != std::string::npos);
    REQUIRE(meta.uniforms.size() == 1);
    CHECK(meta.uniforms[0].default_value.empty());
}

TEST_CASE("A fifth custom sampler is an error where it is declared", "[fjsl]") {
    // Skipping it leaves the name undefined in the generated GLSL, so the
    // real complaint would otherwise arrive as an undeclared identifier at a
    // line the author cannot place.
    auto meta = parse_fjsl("shader_type spatial;\n"
                           "uniform sampler2D a;\n"
                           "uniform sampler2D b;\n"
                           "uniform sampler2D c;\n"
                           "uniform sampler2D d;\n"
                           "uniform sampler2D e;\n"
                           "void fragment() {}");
    REQUIRE(meta.has_errors());
    bool named = false;
    for (const auto& diag : meta.diagnostics) {
        if (diag.message.find("'e'") != std::string::npos) {
            CHECK(diag.line == 6);
            named = true;
        }
    }
    CHECK(named);
}

// --- shader_type ui ---

TEST_CASE("Parse shader_type ui takes its body from fragment()", "[fjsl][ui]") {
    auto meta = parse_fjsl(R"(
        shader_type ui;
        uniform float speed : hint_range(0.0, 3.0, 0.1) = 1.0;
        void fragment() { ALPHA = COLOR.a * step(1.0 - FILL, UV.y); }
    )");
    REQUIRE(meta.shader_type == ShaderType::Ui);
    REQUIRE(meta.fragment_body.code.find("step(1.0 - FILL, UV.y)") != std::string::npos);
    REQUIRE(meta.vertex_body.empty());
    REQUIRE(meta.light_body.empty());
    REQUIRE(meta.custom_param_count == 1);
}

TEST_CASE("shader_type ui rejects the mesh-only hooks", "[fjsl][ui]") {
    require_rejected("shader_type ui;\nvoid vertex() { VERTEX.y += 1.0; }\nvoid fragment() {}");
    require_rejected("shader_type ui;\nvoid light() {}\nvoid fragment() {}");
}

TEST_CASE("shader_type ui reads its material through the quad record", "[fjsl][ui]") {
    auto meta = parse_fjsl(R"(
        shader_type ui;
        uniform vec3 glow : hint_color = vec3(1.0, 0.3, 0.2);
        uniform sampler2D noise;
        void fragment() { ALBEDO = glow * texture(noise, UV).r; }
    )");
    std::string tmpl = "/*FJSL_CUSTOM_TEXTURES*/\n/*FJSL_USER_GLOBALS*/\nvoid main() {\n/*FJSL_USER_CODE*/\n}\n";
    auto result = generate_fragment_shader(meta, tmpl);
    REQUIRE(result.find("materials[quads[frag_quad_index].material_id].param_offset") != std::string::npos);
    REQUIRE(result.find("materials[quads[frag_quad_index].material_id].texture_indices[4]") != std::string::npos);
    REQUIRE(result.find("frag_draw_index") == std::string::npos);
}

// --- Uniforms ---

TEST_CASE("Parse float uniform with default", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float strength = 0.5;
        void fragment() {}
    )");
    REQUIRE(meta.uniforms.size() == 1);
    REQUIRE(meta.uniforms[0].name == "strength");
    REQUIRE(meta.uniforms[0].type == UniformType::Float);
    REQUIRE(meta.uniforms[0].default_value.size() == 1);
    REQUIRE(meta.uniforms[0].default_value[0] == Catch::Approx(0.5f));
}

TEST_CASE("Parse vec3 uniform with hint_color", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 tint : hint_color = vec3(1.0, 0.5, 0.2);
        void fragment() {}
    )");
    REQUIRE(meta.uniforms.size() == 1);
    REQUIRE(meta.uniforms[0].name == "tint");
    REQUIRE(meta.uniforms[0].type == UniformType::Vec3);
    REQUIRE(meta.uniforms[0].hint == UniformHint::Color);
    REQUIRE(meta.uniforms[0].default_value.size() == 3);
    REQUIRE(meta.uniforms[0].default_value[0] == Catch::Approx(1.0f));
    REQUIRE(meta.uniforms[0].default_value[1] == Catch::Approx(0.5f));
    REQUIRE(meta.uniforms[0].default_value[2] == Catch::Approx(0.2f));
}

TEST_CASE("Parse hint_range with min, max, step", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float strength : hint_range(0.0, 1.0, 0.01) = 0.5;
        void fragment() {}
    )");
    REQUIRE(meta.uniforms[0].range.min == Catch::Approx(0.0f));
    REQUIRE(meta.uniforms[0].range.max == Catch::Approx(1.0f));
    REQUIRE(meta.uniforms[0].range.step == Catch::Approx(0.01f));
}

TEST_CASE("Parse sampler2D uniform", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform sampler2D albedo_tex;
        void fragment() {}
    )");
    REQUIRE(meta.uniforms.size() == 1);
    REQUIRE(meta.uniforms[0].type == UniformType::Sampler2D);
    REQUIRE(meta.uniforms[0].slot == 4); // custom textures start at slot 4
    REQUIRE(meta.custom_texture_count == 1);
}

TEST_CASE("Multiple uniforms track param offsets", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float a = 1.0;
        uniform vec3 b = vec3(0.0);
        uniform float c = 0.5;
        void fragment() {}
    )");
    REQUIRE(meta.uniforms.size() == 3);
    REQUIRE(meta.uniforms[0].slot == 0); // float: offset 0
    REQUIRE(meta.uniforms[1].slot == 1); // vec3: offset 1
    REQUIRE(meta.uniforms[2].slot == 4); // float: offset 1+3=4
    REQUIRE(meta.custom_param_count == 5); // 1 + 3 + 1
}

TEST_CASE("Vec3 with single value expands", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 color = vec3(0.5);
        void fragment() {}
    )");
    REQUIRE(meta.uniforms[0].default_value.size() == 3);
    REQUIRE(meta.uniforms[0].default_value[0] == Catch::Approx(0.5f));
    REQUIRE(meta.uniforms[0].default_value[1] == Catch::Approx(0.5f));
    REQUIRE(meta.uniforms[0].default_value[2] == Catch::Approx(0.5f));
}

// --- Uniform hints ---

TEST_CASE("Parse all hint types", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform sampler2D a : hint_white;
        uniform sampler2D b : hint_black;
        uniform sampler2D c : hint_normal;
        uniform sampler2D d : hint_default_mr;
        void fragment() {}
    )");
    REQUIRE(meta.uniforms[0].hint == UniformHint::White);
    REQUIRE(meta.uniforms[1].hint == UniformHint::Black);
    REQUIRE(meta.uniforms[2].hint == UniformHint::Normal);
    REQUIRE(meta.uniforms[3].hint == UniformHint::DefaultMr);
}

TEST_CASE("Hints combine in a comma-separated list", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float strength : hint_range(0.0, 20.0, 0.1), hint_emission = 5.0;
        uniform vec3 glow : hint_color, hint_emission = vec3(1.0, 0.7, 0.2);
        uniform vec3 tint : hint_color = vec3(1.0);
        void fragment() {}
    )");
    REQUIRE(meta.uniforms.size() == 3);

    // Commas inside hint_range's arguments must not split the list.
    const auto& strength = meta.uniforms[0];
    REQUIRE(strength.emission);
    REQUIRE(strength.hint == UniformHint::None);
    REQUIRE(strength.range.min == Catch::Approx(0.0f));
    REQUIRE(strength.range.max == Catch::Approx(20.0f));
    REQUIRE(strength.range.step == Catch::Approx(0.1f));
    REQUIRE(strength.default_value.size() == 1);
    REQUIRE(strength.default_value[0] == Catch::Approx(5.0f));

    // hint_emission sits beside a presentation hint rather than replacing it.
    const auto& glow = meta.uniforms[1];
    REQUIRE(glow.emission);
    REQUIRE(glow.hint == UniformHint::Color);
    REQUIRE(glow.default_value.size() == 3);

    REQUIRE_FALSE(meta.uniforms[2].emission);
    REQUIRE(meta.uniforms[2].hint == UniformHint::Color);
}

TEST_CASE("Unknown hint is ignored rather than fatal", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 tint : hint_typo = vec3(1.0);
        void fragment() {}
    )");
    REQUIRE(meta.uniforms.size() == 1);
    REQUIRE(meta.uniforms[0].hint == UniformHint::None);
    REQUIRE_FALSE(meta.uniforms[0].emission);
}

TEST_CASE("Listed hints are stripped from user globals", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float strength : hint_range(0.0, 20.0, 0.1), hint_emission = 5.0;
        const float HELPER = 2.0;
        void fragment() {}
    )");
    // The strip pass must recognise the same grammar as the metadata pass,
    // or the declaration leaks into generated GLSL as a stray token.
    REQUIRE(meta.user_globals.find("uniform") == std::string::npos);
    REQUIRE(meta.user_globals.find("hint_emission") == std::string::npos);
    REQUIRE(meta.user_globals.find("HELPER") != std::string::npos);
}

// --- render_mode ---

TEST_CASE("Parse render_mode unlit", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        render_mode unlit;
        void fragment() {}
    )");
    REQUIRE(meta.render_mode == RenderMode::Unlit);
}

TEST_CASE("Parse render_mode cull_disabled", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        render_mode cull_disabled;
        void fragment() {}
    )");
    REQUIRE(meta.render_state.cull_disabled);
}

TEST_CASE("Parse multiple render_mode keywords", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        render_mode unlit, cull_disabled, depth_write_disabled, blend_add;
        void fragment() {}
    )");
    REQUIRE(meta.render_mode == RenderMode::Unlit);
    REQUIRE(meta.render_state.cull_disabled);
    REQUIRE(meta.render_state.depth_write_disabled);
    REQUIRE(meta.render_state.blend == BlendMode::Add);
}

// --- Function body extraction ---

TEST_CASE("Extract fragment body", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 tint : hint_color = vec3(1.0, 0.5, 0.2);
        void fragment() {
            ALBEDO = mix(ALBEDO, ALBEDO * tint, 0.5);
        }
    )");
    REQUIRE(meta.fragment_body.code.find("ALBEDO") != std::string::npos);
    REQUIRE(meta.fragment_body.code.find("mix") != std::string::npos);
}

TEST_CASE("Extract vertex body", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        void vertex() {
            VERTEX.y += sin(TIME);
        }
        void fragment() {}
    )");
    REQUIRE(meta.vertex_body.code.find("VERTEX") != std::string::npos);
}

TEST_CASE("Empty fragment body", "[fjsl]") {
    auto meta = parse_fjsl("shader_type spatial;\nvoid fragment() {}");
    REQUIRE(meta.fragment_body.empty());
}

TEST_CASE("No vertex function means empty vertex body", "[fjsl]") {
    auto meta = parse_fjsl("shader_type spatial;\nvoid fragment() {}");
    REQUIRE(meta.vertex_body.empty());
}

TEST_CASE("Nested braces in function body", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        void fragment() {
            if (true) {
                ALBEDO = vec3(1.0);
            }
        }
    )");
    REQUIRE(meta.fragment_body.code.find("if") != std::string::npos);
    REQUIRE(meta.fragment_body.code.find("ALBEDO") != std::string::npos);
}

// --- uniform_float_count ---

TEST_CASE("uniform_float_count returns correct sizes", "[fjsl]") {
    REQUIRE(uniform_float_count(UniformType::Float) == 1);
    REQUIRE(uniform_float_count(UniformType::Vec2) == 2);
    REQUIRE(uniform_float_count(UniformType::Vec3) == 3);
    REQUIRE(uniform_float_count(UniformType::Vec4) == 4);
    REQUIRE(uniform_float_count(UniformType::Sampler2D) == 0);
}

// --- Uniform types and defaults ---

TEST_CASE("Parse vec2 uniform with default", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec2 uv_scale = vec2(1.0, 2.0);
        void fragment() {}
    )");
    REQUIRE(meta.uniforms[0].type == UniformType::Vec2);
    REQUIRE(meta.uniforms[0].default_value.size() == 2);
    REQUIRE(meta.uniforms[0].default_value[0] == Catch::Approx(1.0f));
    REQUIRE(meta.uniforms[0].default_value[1] == Catch::Approx(2.0f));
}

TEST_CASE("Parse vec4 uniform with default", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec4 color = vec4(1.0, 0.5, 0.2, 0.8);
        void fragment() {}
    )");
    REQUIRE(meta.uniforms[0].type == UniformType::Vec4);
    REQUIRE(meta.uniforms[0].default_value.size() == 4);
    REQUIRE(meta.uniforms[0].default_value[3] == Catch::Approx(0.8f));
}

TEST_CASE("Vec4 single value expands to 4 components", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec4 c = vec4(0.5);
        void fragment() {}
    )");
    REQUIRE(meta.uniforms[0].default_value.size() == 4);
    for (int i = 0; i < 4; i++) {
        REQUIRE(meta.uniforms[0].default_value[i] == Catch::Approx(0.5f));
    }
}

TEST_CASE("Vec2 single value expands to 2 components", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec2 s = vec2(3.0);
        void fragment() {}
    )");
    REQUIRE(meta.uniforms[0].default_value.size() == 2);
    REQUIRE(meta.uniforms[0].default_value[0] == Catch::Approx(3.0f));
    REQUIRE(meta.uniforms[0].default_value[1] == Catch::Approx(3.0f));
}

TEST_CASE("Uniform with no default has empty default_value", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float x;
        void fragment() {}
    )");
    REQUIRE(meta.uniforms[0].default_value.empty());
}

TEST_CASE("Uniform with no hint has UniformHint::None", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float x = 1.0;
        void fragment() {}
    )");
    REQUIRE(meta.uniforms[0].hint == UniformHint::None);
}

TEST_CASE("hint_range with only min and max (no step)", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float x : hint_range(0.0, 10.0) = 5.0;
        void fragment() {}
    )");
    REQUIRE(meta.uniforms[0].range.min == Catch::Approx(0.0f));
    REQUIRE(meta.uniforms[0].range.max == Catch::Approx(10.0f));
    REQUIRE(meta.uniforms[0].range.step == Catch::Approx(0.01f)); // default
}

TEST_CASE("Negative default values", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float offset = -1.5;
        void fragment() {}
    )");
    REQUIRE(meta.uniforms[0].default_value[0] == Catch::Approx(-1.5f));
}

// --- Sampler2D limits ---

TEST_CASE("Four sampler2D uniforms fill slots 4-7", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform sampler2D a;
        uniform sampler2D b;
        uniform sampler2D c;
        uniform sampler2D d;
        void fragment() {}
    )");
    REQUIRE(meta.uniforms.size() == 4);
    REQUIRE(meta.uniforms[0].slot == 4);
    REQUIRE(meta.uniforms[1].slot == 5);
    REQUIRE(meta.uniforms[2].slot == 6);
    REQUIRE(meta.uniforms[3].slot == 7);
    REQUIRE(meta.custom_texture_count == 4);
}

TEST_CASE("Fifth sampler2D is skipped", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform sampler2D a;
        uniform sampler2D b;
        uniform sampler2D c;
        uniform sampler2D d;
        uniform sampler2D e;
        void fragment() {}
    )");
    // Only 4 kept, 5th skipped with warning
    REQUIRE(meta.uniforms.size() == 4);
    REQUIRE(meta.custom_texture_count == 4);
}

TEST_CASE("Mixed sampler2D and scalar uniforms", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float strength = 0.5;
        uniform sampler2D tex;
        uniform vec3 tint = vec3(1.0);
        void fragment() {}
    )");
    REQUIRE(meta.uniforms.size() == 3);
    REQUIRE(meta.uniforms[0].slot == 0);  // float at param offset 0
    REQUIRE(meta.uniforms[1].slot == 4);  // sampler at texture slot 4
    REQUIRE(meta.uniforms[2].slot == 1);  // vec3 at param offset 1
    REQUIRE(meta.custom_param_count == 4); // 1 + 3
    REQUIRE(meta.custom_texture_count == 1);
}

// --- render_mode edge cases ---

TEST_CASE("render_mode cull_front", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        render_mode cull_front;
        void fragment() {}
    )");
    REQUIRE(meta.render_state.cull_front);
    REQUIRE_FALSE(meta.render_state.cull_disabled);
}

TEST_CASE("render_mode depth_test_disabled", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        render_mode depth_test_disabled;
        void fragment() {}
    )");
    REQUIRE(meta.render_state.depth_test_disabled);
}

TEST_CASE("render_mode blend_multiply", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        render_mode blend_multiply;
        void fragment() {}
    )");
    REQUIRE(meta.render_state.blend == BlendMode::Multiply);
}

// --- Function body edge cases ---

TEST_CASE("Both vertex and fragment bodies extracted", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        void vertex() {
            VERTEX.y += 1.0;
        }
        void fragment() {
            ALBEDO = vec3(1.0);
        }
    )");
    REQUIRE(meta.vertex_body.code.find("VERTEX") != std::string::npos);
    REQUIRE(meta.fragment_body.code.find("ALBEDO") != std::string::npos);
}

TEST_CASE("Fragment body with deeply nested braces", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        void fragment() {
            if (true) {
                for (int i = 0; i < 3; i++) {
                    if (i == 1) {
                        ALBEDO = vec3(float(i));
                    }
                }
            }
        }
    )");
    REQUIRE(meta.fragment_body.code.find("ALBEDO") != std::string::npos);
    REQUIRE(meta.fragment_body.code.find("for") != std::string::npos);
}

// --- No uniforms / no render_mode ---

TEST_CASE("Shader with no uniforms", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        void fragment() {
            ALBEDO = vec3(1.0, 0.0, 0.0);
        }
    )");
    REQUIRE(meta.uniforms.empty());
    REQUIRE(meta.custom_param_count == 0);
    REQUIRE(meta.custom_texture_count == 0);
}

TEST_CASE("Shader with no render_mode uses defaults", "[fjsl]") {
    auto meta = parse_fjsl("shader_type spatial;\nvoid fragment() {}");
    REQUIRE(meta.render_mode == RenderMode::Lit);
    REQUIRE(meta.render_state.blend == BlendMode::Alpha);
}

// --- Code generation ---

TEST_CASE("generate_fragment_shader substitutes placeholders", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float strength = 0.5;
        void fragment() {
            ALBEDO *= strength;
        }
    )");

    std::string tmpl = R"(
/*FJSL_CUSTOM_TEXTURES*/
void main() {
/*FJSL_CUSTOM_PARAMS*/
/*FJSL_USER_CODE*/
}
)";

    auto result = generate_fragment_shader(meta, tmpl);
    // Custom params should define strength from SSBO
    REQUIRE(result.find("strength") != std::string::npos);
    REQUIRE(result.find("custom_params") != std::string::npos);
    // User code should be injected
    REQUIRE(result.find("ALBEDO *= strength") != std::string::npos);
    // Placeholders should be gone
    REQUIRE(result.find("/*FJSL_CUSTOM_PARAMS*/") == std::string::npos);
    REQUIRE(result.find("/*FJSL_USER_CODE*/") == std::string::npos);
}

TEST_CASE("generate_vertex_shader substitutes placeholders", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float wave_height = 1.0;
        void vertex() {
            VERTEX.y += sin(VERTEX.x) * wave_height;
        }
        void fragment() {}
    )");

    std::string tmpl = R"(
void main() {
/*FJSL_VERTEX_CUSTOM_PARAMS*/
/*FJSL_VERTEX_USER_CODE*/
}
)";

    auto result = generate_vertex_shader(meta, tmpl);
    REQUIRE(result.find("wave_height") != std::string::npos);
    REQUIRE(result.find("VERTEX.y") != std::string::npos);
    REQUIRE(result.find("/*FJSL_VERTEX_CUSTOM_PARAMS*/") == std::string::npos);
}

TEST_CASE("generate_fragment_shader with sampler2D defines texture macro", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform sampler2D detail_tex;
        void fragment() {}
    )");

    std::string tmpl = "/*FJSL_CUSTOM_TEXTURES*/\n/*FJSL_CUSTOM_PARAMS*/\n/*FJSL_USER_CODE*/\n";
    auto result = generate_fragment_shader(meta, tmpl);
    REQUIRE(result.find("#define detail_tex") != std::string::npos);
    REQUIRE(result.find("texture_indices[4]") != std::string::npos);
}

// --- Stress ---

TEST_CASE("Many uniforms", "[fjsl]") {
    std::string src = "shader_type spatial;\n";
    for (int i = 0; i < 20; i++) {
        src += "uniform float u" + std::to_string(i) + " = " + std::to_string(i) + ".0;\n";
    }
    src += "void fragment() {}";

    auto meta = parse_fjsl(src);
    REQUIRE(meta.uniforms.size() == 20);
    REQUIRE(meta.custom_param_count == 20); // 20 floats
    REQUIRE(meta.uniforms[19].slot == 19);
}

// --- Default render state ---

TEST_CASE("Default render state is lit with back-face culling", "[fjsl]") {
    auto meta = parse_fjsl("shader_type spatial;\nvoid fragment() {}");
    REQUIRE(meta.render_mode == RenderMode::Lit);
    REQUIRE_FALSE(meta.render_state.cull_disabled);
    REQUIRE_FALSE(meta.render_state.cull_front);
    REQUIRE_FALSE(meta.render_state.depth_write_disabled);
    REQUIRE(meta.render_state.blend == BlendMode::Alpha);
}

// --- Every spatial shader compiles through the template ---
//
// The engine compiles .fjsl lazily, so a template change is only exercised
// by whichever materials a scene happens to use. This runs the whole shader
// library through the same generate + glslc path so nothing waits to break
// until someone assigns it.

namespace {

std::filesystem::path find_shader_dir() {
    // Both the source tree and the build tree carry shaders/include/; walk
    // up from wherever the test runs until one appears.
    for (auto dir : {std::filesystem::current_path(),
                     platform::executable_path().parent_path()}) {
        for (int i = 0; i < 6; ++i) {
            if (std::filesystem::exists(dir / "shaders" / "include" / "fjell_spatial_template.glsl")) {
                return dir / "shaders";
            }
            if (!dir.has_parent_path() || dir.parent_path() == dir) break;
            dir = dir.parent_path();
        }
    }
    return {};
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Runs a command and returns its exit status plus captured output.
std::pair<int, std::string> run(const std::string& cmd) {
    std::string output;
    int status = platform::run_command(cmd + " 2>&1", output);
    return {status, output};
}

std::string quoted(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

} // namespace

TEST_CASE("Every spatial .fjsl generates GLSL that glslc accepts", "[fjsl][glslc]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");

    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_spatial_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-compile-test";
    std::filesystem::create_directories(out_dir);

    int checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(shader_dir)) {
        if (entry.path().extension() != ".fjsl") continue;
        auto name = entry.path().stem().string();
        INFO("shader: " << name);

        auto meta = parse_fjsl(read_text(entry.path()));
        if (meta.shader_type != ShaderType::Spatial) continue;  // sky shaders use their own template

        auto frag = out_dir / (name + ".frag");
        std::ofstream(frag) << generate_fragment_shader(meta, tmpl);

        auto spv = out_dir / (name + ".spv");
        auto [status, output] = run("glslc --target-env=vulkan1.3 -I "
                                    + quoted(shader_dir / "include") + " "
                                    + quoted(frag) + " -o " + quoted(spv));
        INFO(output);
        REQUIRE(status == 0);
        ++checked;
    }
    REQUIRE(checked > 0);
}

// --- A compiler error names the line the author wrote ---
//
// A fragment() body is spliced into a template hundreds of lines long, at an
// offset that moves with the shader's own uniform count — so without #line
// there is not even a fixed delta between the reported line and the real one.
// This is the test that the mapping survives all the way to glslc, because
// every other check of it is a proxy.

TEST_CASE("A GLSL error reports the .fjsl line, not the generated one", "[fjsl][glslc]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");

    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_spatial_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    // The undeclared identifier is on line 5, and nowhere near where it will
    // land in the generated file.
    const std::string src =
        "shader_type spatial;\n"          // 1
        "\n"                              // 2
        "uniform float amount = 0.5;\n"   // 3
        "void fragment() {\n"             // 4
        "    ALBEDO = no_such_symbol;\n"  // 5
        "}\n";                            // 6

    auto meta = parse_fjsl(src);
    meta.source_path = "probe.fjsl";
    REQUIRE_FALSE(meta.has_errors());

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-line-test";
    std::filesystem::create_directories(out_dir);
    auto frag = out_dir / "probe.frag";
    std::ofstream(frag) << generate_fragment_shader(meta, tmpl);

    auto [status, output] = run("glslc --target-env=vulkan1.3 -I "
                                + quoted(shader_dir / "include") + " "
                                + quoted(frag) + " -o "
                                + quoted(out_dir / "probe.spv"));
    INFO(output);
    REQUIRE(status != 0);
    REQUIRE(output.find("probe.fjsl:5") != std::string::npos);
}

TEST_CASE("Template code keeps its own name after a body", "[fjsl][glslc]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");

    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_spatial_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    // A numeric-only #line leaves the filename set to whatever the last one
    // named, so without restoring it every error after a body would go on
    // blaming the .fjsl for template code the author never wrote.
    auto meta = parse_fjsl("shader_type spatial;\nvoid fragment() { ALBEDO = vec3(1.0); }\n");
    meta.source_path = "probe.fjsl";
    const std::string glsl = generate_fragment_shader(meta, tmpl);

    const auto body = glsl.find("ALBEDO = vec3(1.0)");
    REQUIRE(body != std::string::npos);
    const auto resume = glsl.find("#line", body);
    REQUIRE(resume != std::string::npos);
    // The directive that hands numbering back must name a file, and not the
    // author's.
    const auto eol = glsl.find('\n', resume);
    const std::string directive = glsl.substr(resume, eol - resume);
    INFO("directive: " << directive);
    REQUIRE(directive.find('"') != std::string::npos);
    REQUIRE(directive.find("probe.fjsl") == std::string::npos);
}

// --- Every template running vertex() offers the same built-ins ---
//
// A vertex() body is substituted into six templates: the color and depth
// passes each have a mesh-shader and a vertex-shader path, and shadow and
// outline have one each. They are separate files, so a built-in added to
// one and forgotten in another compiles in the color pass and fails the
// moment the shader is drawn into depth, shadow or an outline. Worse, a
// built-in defined differently between them displaces the same vertex to
// two places: z-fighting against the depth prepass, acne against its own
// shadow.
//
// So compile a body using every documented built-in through all six, and
// a body using none through all six as well, since most shaders name no
// built-in at all and must not pay for the ones they skip.

TEST_CASE("Every vertex() template accepts the same built-ins", "[fjsl][glslc]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");

    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());

    struct Template {
        const char* file;
        const char* stage;
    };
    const Template templates[] = {
        {"fjell_mesh_template.glsl", "mesh"},
        {"fjell_vertex_template.glsl", "vert"},
        {"fjell_depth_mesh_template.glsl", "mesh"},
        {"fjell_depth_vertex_template.glsl", "vert"},
        {"fjell_shadow_vertex_template.glsl", "vert"},
        {"fjell_outline_vertex_template.glsl", "vert"},
    };

    struct Body {
        const char* name;
        const char* source;
    };
    const Body bodies[] = {
        {"every_builtin",
         "VERTEX.y += sin(WORLD_VERTEX.x + TIME) * UV.x;\n"
         "VERTEX += NORMAL * 0.01;\n"
         "VERTEX.xz += (MODEL * vec4(0.0, 0.0, 0.0, 1.0)).xz * 0.0;\n"},
        // Most shaders name no built-in. An unused one must not cost them a
        // warning under -Werror or an unresolved symbol.
        {"no_builtin", "VERTEX.y += 0.5;\n"},
    };

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-vertex-template-test";
    std::filesystem::create_directories(out_dir);

    for (const auto& body : bodies) {
        for (const auto& t : templates) {
            INFO("body: " << body.name << ", template: " << t.file);

            auto tmpl = read_text(shader_dir / "include" / t.file);
            REQUIRE_FALSE(tmpl.empty());

            auto meta = parse_fjsl(std::string("shader_type spatial;\nvoid vertex() {\n")
                                   + body.source + "}\nvoid fragment() {}\n");
            REQUIRE_FALSE(meta.vertex_body.empty());

            std::string name = std::string(body.name) + "_" + t.file;
            auto src = out_dir / name;
            std::ofstream(src) << generate_vertex_shader(meta, tmpl);

            auto spv = out_dir / (name + ".spv");
            auto [status, output] = run("glslc --target-env=vulkan1.3 -fshader-stage="
                                        + std::string(t.stage) + " -I "
                                        + quoted(shader_dir / "include") + " "
                                        + quoted(src) + " -o " + quoted(spv));
            INFO(output);
            REQUIRE(status == 0);
        }
    }
}

// --- shader_type terrain ---

TEST_CASE("Parse shader_type terrain with a layers() hook", "[fjsl][terrain]") {
    auto meta = parse_fjsl(R"(
        shader_type terrain;
        uniform float snow_line : hint_range(0.0, 500.0, 1.0) = 120.0;
        void layers() {
            LAYER_WEIGHTS = mix(LAYER_WEIGHTS, layer_mask(3.0), step(snow_line, WORLD_POS.y));
        }
        void fragment() {
            ROUGHNESS = 1.0;
        }
    )");
    REQUIRE(meta.shader_type == ShaderType::Terrain);
    REQUIRE(meta.layers_body.code.find("layer_mask(3.0)") != std::string::npos);
    REQUIRE(meta.fragment_body.code.find("ROUGHNESS = 1.0") != std::string::npos);
    REQUIRE(meta.vertex_body.empty());
    // The hook bodies are not user globals.
    REQUIRE(meta.user_globals.find("LAYER_WEIGHTS") == std::string::npos);
}

TEST_CASE("Terrain rejects vertex(), other types reject layers()", "[fjsl][terrain]") {
    require_rejected("shader_type terrain;\nvoid vertex() {}\nvoid fragment() {}");
    require_rejected("shader_type spatial;\nvoid layers() {}\nvoid fragment() {}");
    require_rejected("shader_type ui;\nvoid layers() {}\nvoid fragment() {}");
    REQUIRE_NOTHROW(parse_fjsl("shader_type terrain;\nvoid light() {}\nvoid fragment() {}"));
}

// --- shader_type water ---

TEST_CASE("Parse shader_type water", "[fjsl][water]") {
    auto meta = parse_fjsl(R"(
        shader_type water;
        uniform vec3 deep : hint_color = vec3(0.1, 0.2, 0.3);
        uniform float clarity : hint_range(0.0, 1.0, 0.01) = 0.5;
        void fragment() {
            ALBEDO = deep;
        }
    )");
    REQUIRE(meta.shader_type == ShaderType::Water);
    REQUIRE(meta.uniforms.size() == 2);
    REQUIRE(meta.fragment_body.code.find("ALBEDO = deep") != std::string::npos);
    REQUIRE(meta.vertex_body.empty());
}

TEST_CASE("Water rejects vertex() and layers()", "[fjsl][water]") {
    // A water surface is shaped by its .fjwater, generated in the mesh
    // shader from the wave field, so a material has no geometry to move.
    require_rejected("shader_type water;\nvoid vertex() {}\nvoid fragment() {}");
    require_rejected("shader_type water;\nvoid layers() {}\nvoid fragment() {}");
    // light() works as it does for spatial and terrain.
    REQUIRE_NOTHROW(parse_fjsl("shader_type water;\nvoid light() {}\nvoid fragment() {}"));
}

TEST_CASE("Water reaches its material row through the push block", "[fjsl][water]") {
    auto meta = parse_fjsl(R"(
        shader_type water;
        uniform float murk = 0.5;
        void fragment() { ALPHA = murk; }
    )");
    std::string tmpl = "/*FJSL_CUSTOM_TEXTURES*/\n/*FJSL_CUSTOM_PARAMS*/\n/*FJSL_USER_CODE*/\n";
    auto result = generate_fragment_shader(meta, tmpl);
    REQUIRE(result.find("push.water_material_id") != std::string::npos);
}

TEST_CASE("An unlit shader still writes every attachment", "[fjsl][glslc]") {
    // A pass binds four attachments and a fragment that leaves one alone gets
    // undefined values in it. The lit path writes all four; the unlit path
    // replaces that whole block, so it has to write them too. Velocity is the
    // one that bites: undefined values there feed TAA, and the result is a
    // smear that only appears in motion and never in a still frame.
    if (run("glslc --version").first != 0) SKIP("glslc not available");

    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());

    // Every template the unlit substitution can reach. Sky and ui have no PBR
    // lighting block for it to replace, so they are not in this list.
    for (const char* name : {"fjell_spatial_template.glsl",
                             "fjell_water_template.glsl",
                             "fjell_terrain_template.glsl"}) {
        INFO("template: " << name);
        auto tmpl = read_text(shader_dir / "include" / name);
        REQUIRE_FALSE(tmpl.empty());

        const std::string type = std::string(name).find("water") != std::string::npos
            ? "water"
            : (std::string(name).find("terrain") != std::string::npos ? "terrain"
                                                                       : "spatial");
        auto meta = parse_fjsl("shader_type " + type + ";\nrender_mode unlit;\n"
                               "void fragment() { ALBEDO = vec3(0.2); }\n");
        REQUIRE(meta.render_mode == RenderMode::Unlit);

        auto result = generate_fragment_shader(meta, tmpl);
        // Assignments, not mentions: the template declares all four as
        // outputs, so searching for the name finds the declaration whether or
        // not anything ever writes it.
        REQUIRE(result.find("out_color =") != std::string::npos);
        REQUIRE(result.find("out_metallic_roughness =") != std::string::npos);
        REQUIRE(result.find("out_normal =") != std::string::npos);
        REQUIRE(result.find("out_velocity =") != std::string::npos);

        // And it has to compile: the velocity write reaches for the clip
        // positions, which every one of these templates carries.
        auto out_dir = std::filesystem::temp_directory_path() / "fjell-unlit-test";
        std::filesystem::create_directories(out_dir);
        auto frag = out_dir / (std::string(name) + ".frag");
        std::ofstream(frag) << result;
        auto spv = out_dir / (std::string(name) + ".spv");
        auto [status, output] = run("glslc --target-env=vulkan1.3 -fshader-stage=frag -I "
                                    + quoted(shader_dir / "include") + " "
                                    + quoted(frag) + " -o " + quoted(spv));
        INFO(output);
        REQUIRE(status == 0);
    }
}

TEST_CASE("The water template accepts every built-in it offers", "[fjsl][water][glslc]") {
    // The template is the only place a water shader's built-ins exist, and a
    // shader naming one that quietly went away fails at the moment someone
    // assigns the material rather than at build time. So write one that uses
    // all of them and put it through the real template.
    if (run("glslc --version").first != 0) SKIP("glslc not available");

    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_water_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    auto meta = parse_fjsl(R"(
        shader_type water;
        uniform vec3 deep : hint_color = vec3(0.16, 0.19, 0.21);
        uniform float clarity : hint_range(0.0, 1.0, 0.01) = 0.3;
        uniform sampler2D foam_tex : hint_white;
        void light() {
            DIFFUSE_LIGHT += ALBEDO * NdotL * LIGHT_COLOR;
        }
        void fragment() {
            float fade = exp(-WATER_DEPTH * 0.35);
            vec3 behind = texture(SCREEN_TEXTURE, SCREEN_UV).rgb;
            ALBEDO = mix(deep, behind, fade * clarity);
            ALBEDO = mix(ALBEDO, texture(foam_tex, UV * 0.1).rgb, CREST);
            ALBEDO += vec3(WAVE_HEIGHT) * 0.01;
            ALBEDO += FLOOR_POS * 0.0 + WORLD_POS * 0.0 + vec3(WATER_UV, 0.0) * 0.0;
            NORMAL = normalize(WORLD_NORMAL + TBN[0] * 0.01);
            EMISSION = vec3(TIME) * 0.0;
            EMISSION += LIGHT_COLOR * max(dot(NORMAL, LIGHT_DIR), 0.0) * LIGHT_INTENSITY * 0.0;
            ROUGHNESS = 0.06;
            METALLIC = 0.0;
            AO = 1.0;
            ALPHA = mix(0.9, 0.2, fade);
        }
    )");

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-water-template-test";
    std::filesystem::create_directories(out_dir);
    auto frag = out_dir / "water_builtins.frag";
    std::ofstream(frag) << generate_fragment_shader(meta, tmpl);

    auto spv = out_dir / "water_builtins.spv";
    auto [status, output] = run("glslc --target-env=vulkan1.3 -fshader-stage=frag -I "
                                + quoted(shader_dir / "include") + " "
                                + quoted(frag) + " -o " + quoted(spv));
    INFO(output);
    REQUIRE(status == 0);
}

TEST_CASE("Terrain params read the pushed material row", "[fjsl][terrain]") {
    auto meta = parse_fjsl(R"(
        shader_type terrain;
        uniform float slope_start = 0.5;
        uniform sampler2D detail;
        void fragment() {}
    )");
    std::string tmpl = "/*FJSL_CUSTOM_TEXTURES*/\n/*FJSL_LAYERS_USER_CODE*/\n/*FJSL_USER_CODE*/\n";
    auto result = generate_fragment_shader(meta, tmpl);
    REQUIRE(result.find("materials[push.terrain_material_id].param_offset") != std::string::npos);
    REQUIRE(result.find("materials[push.terrain_material_id].texture_indices[4]") != std::string::npos);
}

TEST_CASE("Every terrain .fjsl generates GLSL that glslc accepts", "[fjsl][glslc][terrain]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");

    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_terrain_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-compile-test";
    std::filesystem::create_directories(out_dir);

    // A hand-written sample touches every terrain built-in, so the template
    // stays covered even when the shipped shaders use only some of them.
    std::vector<std::pair<std::string, std::string>> sources;
    sources.emplace_back("terrain_builtins", R"(
        shader_type terrain;
        uniform float snow_line : hint_range(0.0, 500.0, 1.0) = 120.0;
        uniform vec3 tint : hint_color = vec3(1.0, 0.9, 0.8);
        uniform sampler2D detail;
        void layers() {
            LAYER_WEIGHTS = mix(LAYER_WEIGHTS, layer_mask(3.0), smoothstep(snow_line, snow_line + 10.0, WORLD_POS.y));
            TRIPLANAR = max(TRIPLANAR, SLOPE / 90.0);
        }
        void fragment() {
            float d = texture(detail, UV * 0.1).r;
            ALBEDO *= tint * mix(0.9, 1.1, d);
            ROUGHNESS = clamp(ROUGHNESS + 0.1 * SLOPE / 90.0, 0.0, 1.0);
            EMISSION += vec3(0.0);
            AO *= 1.0;
            vec2 s = SCREEN_UV + TERRAIN_UV * 0.0 + vec2(TIME) * 0.0;
            NORMAL = normalize(mix(NORMAL, WORLD_NORMAL, s.x * 0.0));
        }
        void light() {
            DIFFUSE_LIGHT += ALBEDO * NdotL / 3.14159;
        }
    )");
    for (const auto& entry : std::filesystem::directory_iterator(shader_dir)) {
        if (entry.path().extension() != ".fjsl") continue;
        auto text = read_text(entry.path());
        if (parse_fjsl(text).shader_type == ShaderType::Terrain) {
            sources.emplace_back(entry.path().stem().string(), text);
        }
    }
    REQUIRE(sources.size() > 1);  // the engine ships terrain.fjsl

    for (const auto& [name, text] : sources) {
        INFO("shader: " << name);
        auto meta = parse_fjsl(text);
        auto frag = out_dir / (name + ".terrain.frag");
        std::ofstream(frag) << generate_fragment_shader(meta, tmpl);

        auto spv = out_dir / (name + ".terrain.spv");
        auto [status, output] = run("glslc --target-env=vulkan1.3 -I "
                                    + quoted(shader_dir / "include") + " "
                                    + quoted(frag) + " -o " + quoted(spv));
        INFO(output);
        REQUIRE(status == 0);
    }
}

// The ui template is compiled in two shapes by PipelineCache; a shader that
// only names UI built-ins must survive both, and a hand-written sample keeps
// the template covered even when no .fjsl in the tree uses every input.
TEST_CASE("shader_type ui generates GLSL that glslc accepts in both shapes", "[fjsl][glslc][ui]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");

    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_ui_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-compile-test";
    std::filesystem::create_directories(out_dir);

    std::vector<std::pair<std::string, std::string>> sources;
    sources.emplace_back("ui_builtins", R"(
        shader_type ui;
        uniform float speed : hint_range(0.0, 3.0, 0.1) = 1.0;
        uniform vec3 glow : hint_color = vec3(1.0, 0.3, 0.2);
        uniform sampler2D noise;
        const float TIME_SCALE = 2.0;
        float ripple(vec2 p) { return sin(p.x * 12.0 - TIME_SCALE * p.y); }
        void fragment() {
            float wave = sin(UV.x * 12.0 - TIME * speed) * 0.02;
            float surface = FILL + wave + texture(noise, PIXEL / SIZE).r * 0.01;
            float inside = step(1.0 - surface, UV.y);
            vec3 base = HAS_TEXTURE ? texture(TEXTURE, UV).rgb : COLOR.rgb;
            ALBEDO = mix(base, glow, 0.3 + 0.2 * HOVER + 0.1 * ACTIVE + 0.1 * FOCUS)
                   + vec3(ripple(SCREEN_UV)) * 0.01;
            ALPHA = COLOR.a * inside;
        }
    )");
    for (const auto& entry : std::filesystem::directory_iterator(shader_dir)) {
        if (entry.path().extension() != ".fjsl") continue;
        auto text = read_text(entry.path());
        if (parse_fjsl(text).shader_type == ShaderType::Ui) {
            sources.emplace_back(entry.path().stem().string(), text);
        }
    }

    for (const auto& [name, text] : sources) {
        INFO("shader: " << name);
        auto meta = parse_fjsl(text);
        auto frag = out_dir / (name + ".ui.frag");
        std::ofstream(frag) << generate_fragment_shader(meta, tmpl);

        for (const char* define : {"", "-DFJELL_UI_WORLD"}) {
            INFO("defines: " << define);
            auto spv = out_dir / (name + ".ui.spv");
            auto [status, output] = run("glslc --target-env=vulkan1.3 -I "
                                        + quoted(shader_dir / "include") + " " + define + " "
                                        + quoted(frag) + " -o " + quoted(spv));
            INFO(output);
            REQUIRE(status == 0);
        }
    }
}

TEST_CASE("hint_color uniforms are authored in sRGB and stored linear", "[fjsl][color]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec3 tint : hint_color = vec3(0.5, 1.0, 0.0);
        uniform vec4 glow : hint_color = vec4(0.5, 0.5, 0.5, 0.6);
        uniform vec3 offset = vec3(0.5, 0.5, 0.5);
        uniform float amount : hint_range(0.0, 1.0, 0.1) = 0.5;
        void fragment() { ALBEDO = tint + offset * amount + glow.rgb; }
    )");
    auto uniform = [&](const std::string& name) -> const FjslUniform& {
        for (const auto& u : meta.uniforms) if (u.name == name) return u;
        FAIL("no uniform " << name);
        return meta.uniforms.front();
    };

    SECTION("a colour's RGB is decoded from sRGB") {
        std::vector<float> v = uniform("tint").default_value;
        encode_uniform_for_gpu(uniform("tint"), v);
        REQUIRE(v[0] == Catch::Approx(0.2140f).margin(0.0005f));
        REQUIRE(v[1] == Catch::Approx(1.0f));
        REQUIRE(v[2] == 0.0f);
    }

    SECTION("alpha is coverage and stays as authored") {
        std::vector<float> v = uniform("glow").default_value;
        encode_uniform_for_gpu(uniform("glow"), v);
        REQUIRE(v[0] == Catch::Approx(0.2140f).margin(0.0005f));
        REQUIRE(v[2] == Catch::Approx(0.2140f).margin(0.0005f));
        REQUIRE(v[3] == Catch::Approx(0.6f));
    }

    SECTION("vectors and scalars without hint_color pass through") {
        std::vector<float> v = uniform("offset").default_value;
        encode_uniform_for_gpu(uniform("offset"), v);
        REQUIRE(v[0] == Catch::Approx(0.5f));
        std::vector<float> a = uniform("amount").default_value;
        encode_uniform_for_gpu(uniform("amount"), a);
        REQUIRE(a[0] == Catch::Approx(0.5f));
    }

    SECTION("decoding restores the authored value") {
        std::vector<float> v = uniform("tint").default_value;
        encode_uniform_for_gpu(uniform("tint"), v);
        decode_uniform_from_gpu(uniform("tint"), v);
        REQUIRE(v[0] == Catch::Approx(0.5f).epsilon(1e-5f));
        REQUIRE(v[1] == Catch::Approx(1.0f).epsilon(1e-5f));
        REQUIRE(v[2] == 0.0f);
    }
}
