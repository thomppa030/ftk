#include "renderer/resources/fjsl_parser.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <filesystem>
#include <fstream>
#include <map>
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

// The declaration alone decides which slot a shader or material may fill, so
// it has to agree with what the full parse makes of the same file.
TEST_CASE("The declared shader_type is read without a full parse", "[fjsl]") {
    REQUIRE(declared_shader_type("shader_type water;\nvoid fragment() {}\n") == ShaderType::Water);
    REQUIRE(declared_shader_type("// a sea\nshader_type   fog ;") == ShaderType::Fog);
    REQUIRE(parse_fjsl("shader_type fog;").shader_type == ShaderType::Fog);
}

TEST_CASE("A library, a missing or an unknown shader_type declares nothing", "[fjsl]") {
    REQUIRE_FALSE(declared_shader_type("shader_function;\nin float x;\nout float y;\n").has_value());
    REQUIRE_FALSE(declared_shader_type("void fragment() {}\n").has_value());
    REQUIRE_FALSE(declared_shader_type("shader_type particle;").has_value());
    // A file claiming both is a library to the parser, which reports the
    // clash; nothing may be drawn with it either way.
    REQUIRE_FALSE(declared_shader_type("shader_function;\nshader_type spatial;").has_value());
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

TEST_CASE("A sampler past the cap is an error where it is declared", "[fjsl]") {
    // Skipping it leaves the name undefined in the generated GLSL, so the
    // real complaint would otherwise arrive as an undeclared identifier at a
    // line the author cannot place.
    std::string src = "shader_type spatial;\n";
    for (uint32_t i = 0; i <= CUSTOM_TEXTURE_SLOTS; ++i) {
        src += "uniform sampler2D s" + std::to_string(i) + ";\n";
    }
    src += "void fragment() {}";

    auto meta = parse_fjsl(src);
    REQUIRE(meta.has_errors());
    // The one past the cap is named, at its own line — the declarations start
    // on line 2, so the (CUSTOM_TEXTURE_SLOTS)th zero-based index sits there.
    const std::string over = "'s" + std::to_string(CUSTOM_TEXTURE_SLOTS) + "'";
    bool named = false;
    for (const auto& diag : meta.diagnostics) {
        if (diag.message.find(over) != std::string::npos) {
            CHECK(diag.line == CUSTOM_TEXTURE_SLOTS + 2);
            named = true;
        }
    }
    CHECK(named);
}

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

TEST_CASE("An int takes one float of the params and hint_enum names its values", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float before = 1.0;
        uniform int mode : hint_enum("Soft, Hard ,Off") = 2;
        uniform int steps : hint_range(1, 8) = 4;
        uniform float after = 0.5;
        void fragment() {}
    )");
    REQUIRE_FALSE(meta.has_errors());
    REQUIRE(meta.diagnostics.empty());
    REQUIRE(meta.uniforms.size() == 4);

    const auto& mode = meta.uniforms[1];
    REQUIRE(mode.type == UniformType::Int);
    REQUIRE(mode.hint == UniformHint::Enum);
    REQUIRE(mode.enum_options == std::vector<std::string>{"Soft", "Hard", "Off"});
    REQUIRE(mode.default_value == std::vector<float>{2.0f});
    REQUIRE(mode.slot == 1);

    const auto& steps = meta.uniforms[2];
    REQUIRE(steps.hint == UniformHint::None);
    REQUIRE(steps.range.declared);
    REQUIRE(steps.slot == 2);
    REQUIRE(meta.uniforms[3].slot == 3);
    REQUIRE(meta.custom_param_count == 4);
    REQUIRE_FALSE(meta.uniforms[0].range.declared);
}

TEST_CASE("hint_enum belongs on an int and needs its names quoted", "[fjsl]") {
    auto on_float = parse_fjsl(R"(
        shader_type spatial;
        uniform float mode : hint_enum("A,B") = 1.0;
        void fragment() {}
    )");
    REQUIRE_FALSE(on_float.has_errors());
    REQUIRE(on_float.uniforms[0].hint == UniformHint::None);
    REQUIRE(on_float.uniforms[0].enum_options.empty());
    REQUIRE(on_float.diagnostics.size() == 1);
    REQUIRE(on_float.diagnostics[0].message.find("hint_enum") != std::string::npos);

    auto unquoted = parse_fjsl(R"(
        shader_type spatial;
        uniform int mode : hint_enum(A, B) = 1;
        void fragment() {}
    )");
    REQUIRE(unquoted.uniforms[0].hint == UniformHint::None);
    REQUIRE(unquoted.diagnostics.size() == 1);
    REQUIRE(unquoted.diagnostics[0].message.find("quoted") != std::string::npos);
}

TEST_CASE("hint_lut marks a sampler as a colour grading table", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type post;
        uniform sampler2D grade : hint_lut;
        uniform float amount : hint_lut = 1.0;
        void color() { COLOR = lut_sample(grade, COLOR) * amount; }
    )");
    REQUIRE_FALSE(meta.has_errors());
    REQUIRE(meta.uniforms[0].hint == UniformHint::Lut);
    // On anything but a sampler it means nothing, and says so.
    REQUIRE(meta.uniforms[1].hint == UniformHint::None);
    REQUIRE(meta.diagnostics.size() == 1);
    REQUIRE(meta.diagnostics[0].message.find("hint_lut") != std::string::npos);
}

TEST_CASE("An int is read rounded, wherever its uniform is read", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform vec2 offset = vec2(0.0);
        uniform int mode : hint_enum("A,B,C") = 1;
        void vertex() { VERTEX.y += float(mode); }
        void fragment() { if (mode == 2) { ALBEDO = vec3(offset, 0.0); } }
    )");
    REQUIRE_FALSE(meta.has_errors());
    // A track or a blend may leave the stored float between two whole
    // numbers; the shader must see the nearest, not the truncation.
    auto frag = generate_fragment_shader(meta, "/*FJSL_CUSTOM_TEXTURES*/\n/*FJSL_USER_CODE*/\n");
    REQUIRE(frag.find("#define mode (int(round(custom_params[materials[draws[frag_draw_index]"
                      ".material_id].param_offset + 2])))") != std::string::npos);
    REQUIRE(frag.find("#define offset (vec2(custom_params[") != std::string::npos);
    auto vert = generate_vertex_shader(meta, "/*FJSL_VERTEX_CUSTOM_PARAMS*/\n/*FJSL_VERTEX_USER_CODE*/\n");
    REQUIRE(vert.find("int mode = int(round(custom_params[mat.param_offset + 2]));")
            != std::string::npos);
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
    REQUIRE(uniform_float_count(UniformType::Int) == 1);
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

TEST_CASE("A sampler past the cap is not assigned a slot", "[fjsl]") {
    std::string src = "shader_type spatial;\n";
    for (uint32_t i = 0; i <= CUSTOM_TEXTURE_SLOTS; ++i) {
        src += "uniform sampler2D s" + std::to_string(i) + ";\n";
    }
    src += "void fragment() {}";

    auto meta = parse_fjsl(src);
    // Everything up to the cap is kept; the one past it is refused above and
    // never takes a slot, so nothing reads texture_indices past the array.
    REQUIRE(meta.uniforms.size() == CUSTOM_TEXTURE_SLOTS);
    REQUIRE(meta.custom_texture_count == CUSTOM_TEXTURE_SLOTS);
    for (const auto& u : meta.uniforms) {
        REQUIRE(u.slot >= PBR_TEXTURE_SLOTS);
        REQUIRE(u.slot < TOTAL_TEXTURE_SLOTS);
    }
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

// sky() belongs to one type, and the table that refuses a hook is now the same
// one that decides which bodies are stripped from the globals. A type that
// never ran sky() went unguarded before: the body stayed in user_globals and
// reached glslc as a stray function nobody called.
TEST_CASE("Only a sky shader may define sky()", "[fjsl][sky]") {
    require_rejected("shader_type spatial;\nvoid sky() {}\nvoid fragment() {}");
    require_rejected("shader_type ui;\nvoid sky() {}\nvoid fragment() {}");
    require_rejected("shader_type terrain;\nvoid sky() {}\nvoid fragment() {}");
    require_rejected("shader_type water;\nvoid sky() {}\nvoid fragment() {}");
    REQUIRE_NOTHROW(parse_fjsl("shader_type sky;\nvoid sky() {}"));
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

TEST_CASE("Parse shader_type fog takes its body from density()", "[fjsl][fog]") {
    auto meta = parse_fjsl(R"(
        shader_type fog;
        uniform float density : hint_range(0.0, 1.0, 0.001) = 0.003;
        void density() {
            DENSITY = density * exp(-WORLD_POS.y);
            COLOR = vec3(0.8);
        }
        void light() {
            SCATTERED += LIGHT_COLOR * SHADOW;
        }
    )");
    REQUIRE_FALSE(meta.has_errors());
    REQUIRE(meta.shader_type == ShaderType::Fog);
    REQUIRE(meta.fragment_body.code.find("DENSITY = density") != std::string::npos);
    REQUIRE(meta.light_body.code.find("SCATTERED +=") != std::string::npos);
    REQUIRE(meta.uniforms.size() == 1);
    REQUIRE(shader_type_from_name("fog") == ShaderType::Fog);
    REQUIRE(shader_type_name(ShaderType::Fog) == "fog");
}

TEST_CASE("Fog rejects the surface hooks, other types reject density()", "[fjsl][fog]") {
    for (const char* hook : {"fragment", "vertex", "sky", "layers"}) {
        INFO("hook: " << hook);
        auto meta = parse_fjsl("shader_type fog;\nvoid density() {}\nvoid "
                               + std::string(hook) + "() {}\n");
        REQUIRE(meta.has_errors());
        // The refused body is stripped with the permitted ones, so nothing of
        // it reaches the generated file's globals.
        REQUIRE(meta.user_globals.find(hook) == std::string::npos);
    }
    for (const char* type : {"spatial", "sky", "ui", "terrain", "water", "post"}) {
        INFO("type: " << type);
        auto meta = parse_fjsl("shader_type " + std::string(type)
                               + ";\nvoid density() { DENSITY = 1.0; }\n");
        REQUIRE(meta.has_errors());
        bool named = false;
        for (const auto& d : meta.diagnostics) {
            named |= d.message.find("density()") != std::string::npos;
        }
        REQUIRE(named);
    }
    auto function = parse_fjsl("shader_function;\nout float x;\nvoid main() { x = 1.0; }\n"
                               "void density() {}\n");
    REQUIRE(function.has_errors());
}

TEST_CASE("A fog shader without density() is warned about", "[fjsl][fog]") {
    auto meta = parse_fjsl("shader_type fog;\nvoid light() { SCATTERED += LIGHT_COLOR; }\n");
    REQUIRE_FALSE(meta.has_errors());
    REQUIRE(meta.fragment_body.empty());
    bool warned = false;
    for (const auto& d : meta.diagnostics) {
        warned |= d.severity == FjslSeverity::warning
               && d.message.find("density()") != std::string::npos;
    }
    REQUIRE(warned);
}

TEST_CASE("Fog reads its material row through the push block", "[fjsl][fog]") {
    auto meta = parse_fjsl(R"(
        shader_type fog;
        uniform float density = 0.01;
        uniform sampler2D detail;
        void density() { DENSITY = density * texture(detail, WORLD_POS.xz).r; }
    )");
    REQUIRE_FALSE(meta.has_errors());
    const std::string tmpl = "/*FJSL_CUSTOM_TEXTURES*/\nvoid f() {\n/*FJSL_USER_CODE*/\n}\n"
                             "void l() {\n/*FJSL_LIGHT_USER_CODE*/\n}\n";
    auto glsl = generate_fragment_shader(meta, tmpl);
    REQUIRE(glsl.find("materials[pc.fog_material_id].param_offset") != std::string::npos);
    REQUIRE(glsl.find("materials[pc.fog_material_id].texture_indices[4]") != std::string::npos);
    // A fog shader that leaves light() out scatters by the engine's phase
    // lobe, not by the surface default with its normals and Fresnel.
    REQUIRE(glsl.find("henyey_greenstein(0.7, COS_THETA)") != std::string::npos);
    REQUIRE(glsl.find("distribution_GGX") == std::string::npos);
}

// The fog template is compiled as a compute shader by PipelineCache; a
// hand-written sample touches every fog built-in so the template stays
// covered even when the shipped shader uses only some of them.
TEST_CASE("Every fog .fjsl generates GLSL that glslc accepts", "[fjsl][glslc][fog]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");

    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_fog_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-compile-test";
    std::filesystem::create_directories(out_dir);

    std::vector<std::pair<std::string, std::string>> sources;
    sources.emplace_back("fog_builtins", R"(
        shader_type fog;
        uniform float density : hint_range(0.0, 1.0, 0.001) = 0.01;
        uniform vec3 tint : hint_color = vec3(1.0);
        uniform sampler2D mask;
        void density() {
            float h = WORLD_POS.y - ORIGIN.y + LOCAL_POS.y * 0.0;
            float wisps = 1.0 + 0.5 * fbm_noise(WORLD_POS / 8.0 + vec3(WIND_DRIFT.x, 0.0, WIND_DRIFT.y), 2);
            float stirred = 1.0 + STIR + MOTION.recency * MOTION_FALLOFF.sharpness * 0.0;
            float mask_v = texture(mask, WORLD_POS.xz * 0.01).r;
            DENSITY = density * exp(-h) * wisps * stirred * OCCUPANCY * mask_v;
            COLOR = tint * mix(1.0, 0.5, CLOUD_COVERAGE) * mix(1.0, 0.8, PRECIPITATION);
            SCATTERING = 1.0 + 0.0 * (VIEW_DIST + TIME + TIME_OF_DAY + WIND_SPEED
                                      + dot(VIEW_DIR, SUN_DIR) + dot(WIND_DIR, CAMERA_POS)
                                      + dot(MOTION.velocity, vec2(1.0)) + MOTION.height);
        }
        void light() {
            if (LIGHT_KIND == FOG_LIGHT_AMBIENT) {
                SCATTERED += LIGHT_COLOR * ISOTROPIC_PHASE;
                return;
            }
            float phase = henyey_greenstein(0.5, COS_THETA);
            float by_thickness = mix(1.0, 0.5, clamp(DENSITY * 10.0, 0.0, 1.0));
            if (LIGHT_KIND == FOG_LIGHT_SUN) phase *= step(0.5, SHADOW);
            SCATTERED += LIGHT_COLOR * COLOR * phase * SHADOW * by_thickness
                       + 0.0 * (dot(LIGHT_DIR, VIEW_DIR) + WORLD_POS.x);
        }
    )");
    for (const auto& entry : std::filesystem::directory_iterator(shader_dir)) {
        if (entry.path().extension() != ".fjsl") continue;
        auto text = read_text(entry.path());
        if (parse_fjsl(text).shader_type == ShaderType::Fog) {
            sources.emplace_back(entry.path().stem().string(), text);
        }
    }
    REQUIRE(sources.size() > 1);  // the engine ships fog.fjsl

    // One generated source, two compute variants: the medium and the light.
    for (const auto& [name, text] : sources) {
        INFO("shader: " << name);
        auto meta = parse_fjsl(text);
        REQUIRE_FALSE(meta.has_errors());
        auto comp = out_dir / (name + ".fog.comp");
        std::ofstream(comp) << generate_fragment_shader(meta, tmpl);

        for (const char* variant : {"", "-DFJELL_FOG_LIGHT"}) {
            INFO("variant: " << (variant[0] == '\0' ? "medium" : "light"));
            auto spv = out_dir / (name + ".fog.spv");
            auto [status, output] = run("glslc --target-env=vulkan1.3 -fshader-stage=compute "
                                        + std::string(variant) + " -I "
                                        + quoted(shader_dir / "include") + " "
                                        + quoted(comp) + " -o " + quoted(spv));
            INFO(output);
            REQUIRE(status == 0);
        }
    }
}

TEST_CASE("Parse shader_type post takes its body from color()", "[fjsl][post]") {
    auto meta = parse_fjsl(R"(
        shader_type post;
        uniform int operator : hint_enum("Linear,Reinhard,ACES,AgX,Hable,Neutral") = 3;
        uniform float bloom_intensity : hint_range(0.0, 1.0, 0.01) = 0.3;
        void color() {
            COLOR = tonemap(operator, COLOR + BLOOM * bloom_intensity);
        }
    )");
    REQUIRE_FALSE(meta.has_errors());
    REQUIRE(meta.shader_type == ShaderType::Post);
    REQUIRE(meta.fragment_body.code.find("COLOR = tonemap") != std::string::npos);
    REQUIRE(meta.user_globals.find("void color") == std::string::npos);
    REQUIRE(shader_type_from_name("post") == ShaderType::Post);
    REQUIRE(shader_type_name(ShaderType::Post) == "post");
}

TEST_CASE("Post rejects the other hooks, other types reject color()", "[fjsl][post]") {
    for (const char* hook : {"fragment", "vertex", "light", "sky", "layers", "density"}) {
        INFO("hook: " << hook);
        auto meta = parse_fjsl("shader_type post;\nvoid color() {}\nvoid "
                               + std::string(hook) + "() {}\n");
        REQUIRE(meta.has_errors());
        REQUIRE(meta.user_globals.find(hook) == std::string::npos);
    }
    for (const char* type : {"spatial", "sky", "ui", "terrain", "water", "fog"}) {
        INFO("type: " << type);
        auto meta = parse_fjsl("shader_type " + std::string(type)
                               + ";\nvoid color() { COLOR = vec3(1.0); }\n");
        REQUIRE(meta.has_errors());
        bool named = false;
        for (const auto& d : meta.diagnostics) {
            named |= d.message.find("display colour") != std::string::npos;
        }
        REQUIRE(named);
    }
    auto function = parse_fjsl("shader_function;\nout float x;\nvoid main() { x = 1.0; }\n"
                               "void color() {}\n");
    REQUIRE(function.has_errors());
}

TEST_CASE("A post shader without color() is warned about", "[fjsl][post]") {
    auto meta = parse_fjsl("shader_type post;\nuniform float x = 1.0;\n");
    REQUIRE_FALSE(meta.has_errors());
    bool warned = false;
    for (const auto& d : meta.diagnostics) {
        warned |= d.severity == FjslSeverity::warning
               && d.message.find("color()") != std::string::npos;
    }
    REQUIRE(warned);
}

TEST_CASE("Post reads its values from the pass and its textures from the row", "[fjsl][post]") {
    auto meta = parse_fjsl(R"(
        shader_type post;
        uniform vec3 tint : hint_color = vec3(1.0);
        uniform int mode : hint_enum("A,B") = 1;
        uniform sampler2D grain;
        void color() { COLOR *= tint * texture(grain, SCREEN_UV).r * float(mode); }
    )");
    REQUIRE_FALSE(meta.has_errors());
    auto glsl = generate_fragment_shader(meta, "/*FJSL_CUSTOM_TEXTURES*/\n/*FJSL_USER_CODE*/\n");
    // The values differ per viewport, so they come from the pass's buffer at
    // the pushed base, never from the material row's slice of the params.
    REQUIRE(glsl.find("#define tint (vec3(post_params[push.param_base + 0], "
                      "post_params[push.param_base + 1], post_params[push.param_base + 2]))")
            != std::string::npos);
    REQUIRE(glsl.find("#define mode (int(round(post_params[push.param_base + 3])))")
            != std::string::npos);
    REQUIRE(glsl.find("custom_params") == std::string::npos);
    REQUIRE(glsl.find("materials[push.material_id].texture_indices[4]") != std::string::npos);
}

// The post template is drawn by the post pass as a fullscreen fragment; a
// hand-written sample touches every built-in and helper so the template stays
// covered even when the shipped shader uses only some of them.
TEST_CASE("Every post .fjsl generates GLSL that glslc accepts", "[fjsl][glslc][post]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");

    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_post_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-compile-test";
    std::filesystem::create_directories(out_dir);

    std::vector<std::pair<std::string, std::string>> sources;
    sources.emplace_back("post_builtins", R"(
        shader_type post;
        uniform int operator : hint_enum("Linear,Reinhard,ACES,AgX,Hable,Neutral") = 3;
        uniform vec3 tint : hint_color = vec3(1.0);
        uniform sampler2D grain;
        vec3 vignette(vec2 uv) { return vec3(1.0 - 0.3 * dot(uv - 0.5, uv - 0.5)); }
        void color() {
            vec3 c = COLOR + (HAS_BLOOM ? BLOOM : vec3(0.0));
            float far = smoothstep(50.0, 400.0, DEPTH) + texture(DEPTH_TEXTURE, SCREEN_UV).r * 0.0;
            float weather = CLOUD_COVERAGE + PRECIPITATION + WIND_SPEED + dot(WIND_DIR, SUN_DIR);
            float clock = TIME + TIME_OF_DAY + EXPOSURE + LUMINANCE;
            vec2 at = PIXEL / VIEWPORT_SIZE;
            c = tonemap(operator, c) + 0.0 * (tonemap_agx(c) + tonemap_aces(c) + tonemap_hable(c)
                                              + tonemap_reinhard(c) + tonemap_neutral(c));
            c = srgb_to_linear(linear_to_srgb(c)) * tint * vignette(at);
            vec3 around = scene_at(SCREEN_UV + TEXEL_SIZE) + texture(SCENE_TEXTURE, SCREEN_UV).rgb
                        + normal_at(SCREEN_UV) * depth_at(SCREEN_UV) + vec3(velocity_at(SCREEN_UV), 0.0);
            vec3 effects = sharpen(SCREEN_UV, 0.5) + kuwahara(SCREEN_UV, 2)
                         + chromatic_aberration(SCREEN_UV, 0.003)
                         + vec3(outline(SCREEN_UV, 0.1, 0.3) + vignette(SCREEN_UV, 0.3, 0.5)
                                + film_grain(PIXEL, TIME, 0.02) + luma_709(c));
            c = posterize(c, 4.0) + 0.0 * (around + effects);
            COLOR = mix(c, vec3(texture(grain, SCREEN_UV).r), 0.0 * (far + weather + clock));
            if (operator == TONEMAP_LINEAR) COLOR = clamp(COLOR, 0.0, 1.0);
        }
    )");
    for (const auto& entry : std::filesystem::directory_iterator(shader_dir)) {
        if (entry.path().extension() != ".fjsl") continue;
        auto text = read_text(entry.path());
        if (parse_fjsl(text).shader_type == ShaderType::Post) {
            sources.emplace_back(entry.path().stem().string(), text);
        }
    }
    REQUIRE(sources.size() > 1);  // the engine ships post.fjsl

    for (const auto& [name, text] : sources) {
        INFO("shader: " << name);
        auto meta = parse_fjsl(text);
        REQUIRE_FALSE(meta.has_errors());
        auto frag = out_dir / (name + ".post.frag");
        std::ofstream(frag) << generate_fragment_shader(meta, tmpl);
        auto spv = out_dir / (name + ".post.spv");
        auto [status, output] = run("glslc --target-env=vulkan1.3 -I "
                                    + quoted(shader_dir / "include") + " "
                                    + quoted(frag) + " -o " + quoted(spv));
        INFO(output);
        REQUIRE(status == 0);
    }
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

// --- shader_function ---
//
// A .fjsl that declares `shader_function;` instead of a `shader_type` is a
// pure function: no pass draws it, it is composed into other files. It has
// typed `in`/`out` ports and one entry point, main().

TEST_CASE("Parse a shader_function with ports", "[fjsl][function]") {
    auto meta = parse_fjsl(
        "shader_function;\n"
        "in vec3 view_dir;\n"
        "in float size;\n"
        "out vec3 color;\n"
        "out float alpha;\n"
        "void main() { color = view_dir; alpha = size; }\n");
    REQUIRE_FALSE(meta.has_errors());
    REQUIRE(meta.is_function);
    REQUIRE_FALSE(meta.type().has_value());
    REQUIRE(meta.inputs.size() == 2);
    REQUIRE(meta.inputs[0].name == "view_dir");
    REQUIRE(meta.inputs[0].type == UniformType::Vec3);
    REQUIRE(meta.inputs[1].type == UniformType::Float);
    REQUIRE(meta.outputs.size() == 2);
    REQUIRE(meta.outputs[0].name == "color");
    REQUIRE_FALSE(meta.main_body.empty());
}

TEST_CASE("A shader_function needs main() and at least one out", "[fjsl][function]") {
    require_rejected("shader_function;\nin vec3 d;\nout vec3 c;");          // no main
    require_rejected("shader_function;\nin vec3 d;\nvoid main() {}");        // no out
}

TEST_CASE("A shader_function rejects a Type's hooks", "[fjsl][function]") {
    require_rejected("shader_function;\nout vec3 c;\nvoid main() {}\nvoid fragment() {}");
    require_rejected("shader_function;\nout vec3 c;\nvoid main() {}\nvoid sky() {}");
    require_rejected("shader_function;\nout vec3 c;\nvoid main() {}\nvoid light() {}");
}

TEST_CASE("A file is a shader_function or a shader_type, not both", "[fjsl][function]") {
    require_rejected("shader_function;\nshader_type spatial;\nout vec3 c;\nvoid main() {}");
    // A shader does not declare ports: its inputs are its Type's built-ins.
    require_rejected("shader_type spatial;\nin vec3 d;\nvoid fragment() {}");
}

TEST_CASE("Port names must not collide", "[fjsl][function]") {
    require_rejected("shader_function;\nin vec3 d;\nin vec3 d;\nout vec3 c;\nvoid main() {}");
    require_rejected("shader_function;\nin vec3 d;\nout vec3 d;\nvoid main() {}");
}

TEST_CASE("A sampler cannot be a port", "[fjsl][function]") {
    // A texture belongs to whichever file samples it, as its own uniform, so
    // it reaches the GPU through that file's material row.
    require_rejected("shader_function;\nin sampler2D t;\nout vec3 c;\nvoid main() {}");
}

TEST_CASE("Short keywords do not match inside other words", "[fjsl][function]") {
    // `in` and `out` are two and three letters; the scan must not read `int`,
    // an identifier containing them, or prose in a comment as a declaration.
    auto meta = parse_fjsl(
        "shader_type spatial;\n"
        "// a point in space, using the outer edge\n"
        "float outer(float x) { return x; }\n"
        "void fragment() { int count = 1; vec3 in_dir = vec3(0.0); ALBEDO = in_dir; }\n");
    REQUIRE_FALSE(meta.has_errors());
    REQUIRE(meta.inputs.empty());
    REQUIRE(meta.outputs.empty());
}

// --- use ---

TEST_CASE("Parse a use wiring", "[fjsl][use]") {
    auto meta = parse_fjsl(
        "shader_type sky;\n"
        "use sun = \"lib/sun_disc.fjsl\"(view_dir: VIEW_DIR, sun_dir: SUN_DIR);\n"
        "void sky() { SKY = sun.color; }\n");
    REQUIRE_FALSE(meta.has_errors());
    REQUIRE(meta.uses.size() == 1);
    REQUIRE(meta.uses[0].name == "sun");
    REQUIRE(meta.uses[0].path == "lib/sun_disc.fjsl");
    REQUIRE(meta.uses[0].arguments.size() == 2);
    REQUIRE(meta.uses[0].arguments[0].first == "view_dir");
    REQUIRE(meta.uses[0].arguments[0].second == "VIEW_DIR");
    REQUIRE(meta.uses[0].arguments[1].second == "SUN_DIR");
}

TEST_CASE("A use argument may be any expression", "[fjsl][use]") {
    // An argument is consumer-side GLSL and carries its own parentheses and
    // commas. Matching the list with a `[^)]*` regex stops at the first ')'
    // and drops the whole declaration without a word.
    auto meta = parse_fjsl(
        "shader_type sky;\n"
        "use sun = \"lib/s.fjsl\"(dir: mix(VIEW_DIR, SUN_DIR, 0.5), k: f(g(1.0)));\n"
        "void sky() {}\n");
    REQUIRE_FALSE(meta.has_errors());
    REQUIRE(meta.uses.size() == 1);
    REQUIRE(meta.uses[0].arguments.size() == 2);
    REQUIRE(meta.uses[0].arguments[0].second == "mix(VIEW_DIR, SUN_DIR, 0.5)");
    REQUIRE(meta.uses[0].arguments[1].second == "f(g(1.0))");
}

TEST_CASE("A use can only read a use declared above it", "[fjsl][use]") {
    // The calls run in source order, so an argument reading a use wired below
    // it — or the one it belongs to — reads a variable nothing has filled.
    auto feeds_forward = parse_fjsl(
        "shader_type sky;\n"
        "use galaxy = \"lib/g.fjsl\"(dir: VIEW_DIR);\n"
        "use stars = \"lib/s.fjsl\"(dir: VIEW_DIR, crowding: galaxy.band);\n"
        "void sky() {}\n");
    REQUIRE_FALSE(feeds_forward.has_errors());

    require_rejected("shader_type sky;\n"
                     "use stars = \"lib/s.fjsl\"(dir: VIEW_DIR, crowding: galaxy.band);\n"
                     "use galaxy = \"lib/g.fjsl\"(dir: VIEW_DIR);\n"
                     "void sky() {}\n");
    require_rejected("shader_type sky;\n"
                     "use stars = \"lib/s.fjsl\"(dir: VIEW_DIR, crowding: stars.band);\n"
                     "void sky() {}\n");

    // A name that merely contains a use's name is not a read of it.
    auto lookalike = parse_fjsl(
        "shader_type sky;\n"
        "use stars = \"lib/s.fjsl\"(dir: VIEW_DIR, crowding: my_stars.x);\n"
        "void sky() {}\n");
    REQUIRE_FALSE(lookalike.has_errors());
}

TEST_CASE("A malformed use is reported", "[fjsl][use]") {
    require_rejected("shader_type sky;\nuse a = \"x.fjsl\"(V);\nvoid sky() {}");
    require_rejected("shader_type sky;\nuse a = \"x.fjsl\"(d: V)\nvoid sky() {}");
    require_rejected("shader_type sky;\nuse a = \"x.fjsl\"(d: V);\n"
                     "use a = \"y.fjsl\"(d: V);\nvoid sky() {}");
}

// --- resolve_composition ---
//
// Wiring is checked before anything is generated: every path resolves to a
// shader_function, every `in` is filled exactly once, and nothing uses itself.
// The reader is a callback so this needs no files on disk — and so the engine
// can resolve project-then-engine while fjlint resolves however it likes.

namespace {

FjslComposition compose(const std::string& root,
                        std::map<std::string, std::string> files) {
    auto meta = parse_fjsl(root);
    meta.source_text = root;
    return resolve_composition(meta, "root.fjsl",
                               [files = std::move(files)](const std::string& path)
                                   -> std::optional<std::string> {
        auto it = files.find(path);
        if (it == files.end()) return std::nullopt;
        return it->second;
    });
}

const std::string SUN_FN =
    "shader_function;\nin vec3 dir;\nout vec3 color;\nvoid main() { color = dir; }";
const std::string NOISE_FN =
    "shader_function;\nin vec2 p;\nout float v;\nvoid main() { v = p.x; }";

} // namespace

TEST_CASE("A composition lists dependencies before the file that uses them",
          "[fjsl][compose]") {
    // The generator emits in this order, so a function's helpers are declared
    // before the code that calls them.
    const std::string deep =
        "shader_function;\nin vec2 p;\nout float v;\n"
        "use n = \"lib/noise.fjsl\"(p: p);\nvoid main() { v = n.v; }";
    auto comp = compose("shader_type sky;\n"
                        "use d = \"lib/deep.fjsl\"(p: P);\nvoid sky() {}",
                        {{"lib/deep.fjsl", deep}, {"lib/noise.fjsl", NOISE_FN}});
    REQUIRE_FALSE(comp.has_errors());
    REQUIRE(comp.files.size() == 3);
    REQUIRE(comp.files[0].path == "lib/noise.fjsl");
    REQUIRE(comp.files[1].path == "lib/deep.fjsl");
    REQUIRE(comp.files[2].path == "root.fjsl");
}

TEST_CASE("A file reached twice appears once", "[fjsl][compose]") {
    const std::string deep =
        "shader_function;\nin vec2 p;\nout float v;\n"
        "use n = \"lib/noise.fjsl\"(p: p);\nvoid main() { v = n.v; }";
    auto comp = compose("shader_type sky;\n"
                        "use d = \"lib/deep.fjsl\"(p: P);\n"
                        "use n = \"lib/noise.fjsl\"(p: P);\nvoid sky() {}",
                        {{"lib/deep.fjsl", deep}, {"lib/noise.fjsl", NOISE_FN}});
    REQUIRE_FALSE(comp.has_errors());
    REQUIRE(comp.files.size() == 3);  // not 4: noise is emitted once
}

TEST_CASE("A cycle is reported with the chain that made it", "[fjsl][compose]") {
    const std::string a = "shader_function;\nin float x;\nout float y;\n"
                          "use b = \"b.fjsl\"(x: x);\nvoid main() { y = b.y; }";
    const std::string b = "shader_function;\nin float x;\nout float y;\n"
                          "use a = \"a.fjsl\"(x: x);\nvoid main() { y = a.y; }";
    auto comp = compose("shader_type sky;\nuse a = \"a.fjsl\"(x: X);\nvoid sky() {}",
                        {{"a.fjsl", a}, {"b.fjsl", b}});
    REQUIRE(comp.has_errors());

    // Using yourself is the shortest cycle and must be caught too.
    auto self = compose("shader_type sky;\nuse r = \"root.fjsl\"(x: X);\nvoid sky() {}",
                        {{"root.fjsl", SUN_FN}});
    REQUIRE(self.has_errors());
}

TEST_CASE("Every in is filled exactly once", "[fjsl][compose]") {
    auto missing = compose("shader_type sky;\nuse s = \"sun.fjsl\"();\nvoid sky() {}",
                           {{"sun.fjsl", SUN_FN}});
    REQUIRE(missing.has_errors());

    auto unknown = compose(
        "shader_type sky;\nuse s = \"sun.fjsl\"(dir: V, bogus: W);\nvoid sky() {}",
        {{"sun.fjsl", SUN_FN}});
    REQUIRE(unknown.has_errors());

    auto good = compose("shader_type sky;\nuse s = \"sun.fjsl\"(dir: V);\nvoid sky() {}",
                        {{"sun.fjsl", SUN_FN}});
    REQUIRE_FALSE(good.has_errors());
}

TEST_CASE("A use must name a readable shader_function", "[fjsl][compose]") {
    auto absent = compose("shader_type sky;\nuse s = \"gone.fjsl\"(dir: V);\nvoid sky() {}",
                          {});
    REQUIRE(absent.has_errors());

    auto shader = compose("shader_type sky;\nuse s = \"m.fjsl\"(dir: V);\nvoid sky() {}",
                          {{"m.fjsl", "shader_type spatial;\nvoid fragment() {}"}});
    REQUIRE(shader.has_errors());
}

TEST_CASE("A used function's own errors name that function", "[fjsl][compose]") {
    // Not the file that wired it: the author of the broken function is who
    // needs to know, and the line number is that file's.
    auto comp = compose("shader_type sky;\nuse s = \"bad.fjsl\"(d: V);\nvoid sky() {}",
                        {{"bad.fjsl", "shader_function;\nin vec3 d;\nout vec3 c;"}});
    REQUIRE(comp.has_errors());
    const bool named = std::any_of(
        comp.diagnostics.begin(), comp.diagnostics.end(),
        [](const FjslDiagnostic& d) { return d.file == "bad.fjsl"; });
    REQUIRE(named);
}

TEST_CASE("A use chain has a depth cap", "[fjsl][compose]") {
    // Everything is inlined into one program, so depth costs compile time and
    // generated size. A chain this long is a mistake, not a design.
    auto chain = [](int n) {
        std::map<std::string, std::string> files;
        for (int i = 0; i < n; ++i) {
            const std::string self = "f" + std::to_string(i) + ".fjsl";
            const std::string next = "f" + std::to_string(i + 1) + ".fjsl";
            files[self] = i + 1 < n
                ? "shader_function;\nin float x;\nout float y;\n"
                  "use u = \"" + next + "\"(x: x);\nvoid main() { y = u.y; }"
                : "shader_function;\nin float x;\nout float y;\nvoid main() { y = x; }";
        }
        return compose("shader_type sky;\nuse a = \"f0.fjsl\"(x: X);\nvoid sky() {}",
                       std::move(files));
    };
    REQUIRE_FALSE(chain(15).has_errors());
    REQUIRE(chain(25).has_errors());
}

TEST_CASE("A composed sky generates GLSL that glslc accepts", "[fjsl][compose][glslc]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");
    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_sky_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    // Two functions that deliberately collide: same helper name, same uniform
    // name. A uniform becomes a file-scope #define and a macro has no scope,
    // so without mangling the second silently wins for both.
    const std::string sun =
        "shader_function;\n"
        "in vec3 view_dir;\nin vec3 sun_dir;\nout vec3 color;\nout float alpha;\n"
        "uniform float size : hint_range(0.1, 4.0, 0.01) = 1.8;\n"
        "float falloff(float d) { return 1.0 - smoothstep(size * 0.98, size, d); }\n"
        "void main() {\n"
        "    float d = degrees(acos(clamp(dot(view_dir, sun_dir), -1.0, 1.0)));\n"
        "    color = vec3(1.0); alpha = falloff(d);\n"
        "}\n";
    const std::string glow =
        "shader_function;\n"
        "in vec3 view_dir;\nout vec3 color;\n"
        "uniform float size : hint_range(0.0, 10.0, 0.1) = 3.0;\n"
        "float falloff(float d) { return exp(-d * size); }\n"
        "void main() { color = vec3(falloff(view_dir.y)); }\n";
    const std::string root =
        "shader_type sky;\n"
        "use sun = \"lib/sun.fjsl\"(view_dir: VIEW_DIR, sun_dir: SUN_DIR);\n"
        "use glow = \"lib/glow.fjsl\"(view_dir: VIEW_DIR);\n"
        "uniform vec3 horizon : hint_color = vec3(0.7, 0.8, 0.9);\n"
        "void sky() {\n"
        "    SKY = horizon;\n"
        "    SKY += sun.color * sun.alpha;\n"
        "    SKY += glow.color * 0.1;\n"
        "}\n";

    auto meta = parse_fjsl(root);
    meta.source_text = root;
    REQUIRE_FALSE(meta.has_errors());
    auto comp = compose(root, {{"lib/sun.fjsl", sun}, {"lib/glow.fjsl", glow}});
    REQUIRE_FALSE(comp.has_errors());

    const std::string glsl = generate_fragment_shader(meta, tmpl, &comp);

    // Both helpers survive under their own names, and neither file's uniform
    // reads the other's storage: each occupies its own run of the consumer's
    // param block, after the consumer's own three floats.
    REQUIRE(glsl.find("sun_falloff") != std::string::npos);
    REQUIRE(glsl.find("glow_falloff") != std::string::npos);
    REQUIRE(glsl.find("param_offset + 3]") != std::string::npos);
    REQUIRE(glsl.find("param_offset + 4]") != std::string::npos);
    // The wiring is gone from the code and became calls.
    REQUIRE(glsl.find("shader_function") == std::string::npos);
    REQUIRE(glsl.find("sun_main(VIEW_DIR, SUN_DIR)") != std::string::npos);

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-compose-test";
    std::filesystem::create_directories(out_dir);
    auto frag = out_dir / "composed.frag";
    std::ofstream(frag) << glsl;
    auto [status, output] = run("glslc --target-env=vulkan1.3 -I "
                                + quoted(shader_dir / "include") + " "
                                + quoted(frag) + " -o "
                                + quoted(out_dir / "composed.spv"));
    INFO(output);
    REQUIRE(status == 0);
}

TEST_CASE("One use feeds another through its arguments", "[fjsl][compose][glslc]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");
    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_sky_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    const std::string galaxy =
        "shader_function;\n"
        "in vec3 dir;\nout vec3 color;\nout float band;\n"
        "void main() { band = 1.0 - abs(dir.y); color = vec3(band); }\n";
    const std::string stars =
        "shader_function;\n"
        "in vec3 dir;\nin float crowding;\nout vec3 color;\n"
        "void main() { color = vec3(crowding * dir.x); }\n";
    const std::string root =
        "shader_type sky;\n"
        "use galaxy = \"lib/galaxy.fjsl\"(dir: VIEW_DIR);\n"
        "use stars = \"lib/stars.fjsl\"(dir: VIEW_DIR, crowding: galaxy.band * 0.5);\n"
        "void sky() { SKY = galaxy.color + stars.color; }\n";

    auto meta = parse_fjsl(root);
    meta.source_text = root;
    REQUIRE_FALSE(meta.has_errors());
    auto comp = compose(root, {{"lib/galaxy.fjsl", galaxy}, {"lib/stars.fjsl", stars}});
    REQUIRE_FALSE(comp.has_errors());

    // The argument names the variable the first function wrote, exactly as
    // the same words in the body would.
    const std::string glsl = generate_fragment_shader(meta, tmpl, &comp);
    REQUIRE(glsl.find("stars_main(VIEW_DIR, galaxy_band * 0.5)") != std::string::npos);
    REQUIRE(glsl.find("galaxy_main(VIEW_DIR)") < glsl.find("stars_main(VIEW_DIR"));

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-compose-test";
    std::filesystem::create_directories(out_dir);
    auto frag = out_dir / "chained.frag";
    std::ofstream(frag) << glsl;
    auto [status, output] = run("glslc --target-env=vulkan1.3 -I "
                                + quoted(shader_dir / "include") + " "
                                + quoted(frag) + " -o "
                                + quoted(out_dir / "chained.spv"));
    INFO(output);
    REQUIRE(status == 0);
}

// The engine's own skies, with whatever they compose out of shaders/lib/. A
// sky is compiled twice — once as the fragment shader that draws it and once
// as the compute shader that fills the lighting cubemap — and a function that
// is fine in one can be illegal in the other: a screen-space derivative, for
// one, has no meaning in compute.
TEST_CASE("Every shipped sky composes and compiles both ways", "[fjsl][compose][glslc]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");
    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_sky_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-sky-test";
    std::filesystem::create_directories(out_dir);

    int checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(shader_dir)) {
        if (entry.path().extension() != ".fjsl") continue;
        auto name = entry.path().stem().string();
        INFO("shader: " << name);

        const std::string source = read_text(entry.path());
        auto meta = parse_fjsl(source);
        if (meta.shader_type != ShaderType::Sky) continue;
        meta.source_text = source;
        REQUIRE_FALSE(meta.has_errors());

        // A `use` names its file from the engine root, as "shaders/lib/x.fjsl".
        auto comp = resolve_composition(
            meta, "shaders/" + entry.path().filename().string(),
            [&](const std::string& path) -> std::optional<std::string> {
                auto text = read_text(shader_dir.parent_path() / path);
                if (text.empty()) return std::nullopt;
                return text;
            });
        for (const auto& d : comp.diagnostics) INFO(d.file << ": " << d.message);
        REQUIRE_FALSE(comp.has_errors());

        auto frag = out_dir / (name + ".frag");
        std::ofstream(frag) << generate_fragment_shader(meta, tmpl, &comp);

        const std::string includes = "-I " + quoted(shader_dir / "include") + " ";
        auto [drawn, drawn_output] = run("glslc --target-env=vulkan1.3 " + includes
                                         + quoted(frag) + " -o "
                                         + quoted(out_dir / (name + ".spv")));
        INFO(drawn_output);
        REQUIRE(drawn == 0);

        auto [baked, baked_output] = run("glslc --target-env=vulkan1.3 -fshader-stage=compute "
                                         "-DFJELL_SKY_CUBEMAP " + includes
                                         + quoted(frag) + " -o "
                                         + quoted(out_dir / (name + ".cubemap.spv")));
        INFO(baked_output);
        REQUIRE(baked == 0);
        ++checked;
    }
    REQUIRE(checked > 0);
}

TEST_CASE("A composed function's textures take the consumer's slots", "[fjsl][compose]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");
    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_sky_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    // Two functions with a texture each, so they must not land on one slot.
    const std::string ramp =
        "shader_function;\n"
        "in vec3 view_dir;\nout vec3 color;\n"
        "uniform float scale : hint_range(0.0, 4.0, 0.1) = 1.0;\n"
        "uniform sampler2D ramp : hint_black;\n"
        "void main() { color = texture(ramp, vec2(view_dir.y * scale, 0.5)).rgb; }\n";
    const std::string grain =
        "shader_function;\n"
        "in vec3 view_dir;\nout vec3 color;\n"
        "uniform sampler2D noise : hint_black;\n"
        "void main() { color = texture(noise, view_dir.xy).rgb; }\n";
    const std::string root =
        "shader_type sky;\n"
        "use aurora = \"lib/ramp.fjsl\"(view_dir: VIEW_DIR);\n"
        "use grain = \"lib/grain.fjsl\"(view_dir: VIEW_DIR);\n"
        "uniform vec3 tint : hint_color = vec3(1.0);\n"
        "uniform sampler2D own : hint_black;\n"
        "void sky() { SKY = tint * texture(own, VIEW_DIR.xy).rgb + aurora.color + grain.color; }\n";

    auto meta = parse_fjsl(root);
    meta.source_text = root;
    REQUIRE_FALSE(meta.has_errors());
    auto comp = compose(root, {{"lib/ramp.fjsl", ramp}, {"lib/grain.fjsl", grain}});
    REQUIRE_FALSE(comp.has_errors());

    const std::string glsl = generate_fragment_shader(meta, tmpl, &comp);

    // The consumer's own sampler keeps slot 4, the first after the PBR set,
    // and each function's follows. Landing two on one slot is the failure this
    // guards: valid GLSL, one picture where two were meant.
    REQUIRE(glsl.find("own textures[nonuniformEXT(materials[push.sky_material_id]"
                      ".texture_indices[4])]") != std::string::npos);
    REQUIRE(glsl.find("aurora_ramp textures[nonuniformEXT(materials[push.sky_material_id]"
                      ".texture_indices[5])]") != std::string::npos);
    REQUIRE(glsl.find("grain_noise textures[nonuniformEXT(materials[push.sky_material_id]"
                      ".texture_indices[6])]") != std::string::npos);
    // The scalar still lands in the parameter block after the consumer's own.
    REQUIRE(glsl.find("param_offset + 3]") != std::string::npos);

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-texslot-test";
    std::filesystem::create_directories(out_dir);
    auto frag = out_dir / "composed.frag";
    std::ofstream(frag) << glsl;
    auto [status, output] = run("glslc --target-env=vulkan1.3 -I "
                                + quoted(shader_dir / "include") + " "
                                + quoted(frag) + " -o " + quoted(out_dir / "c.spv"));
    INFO(output);
    REQUIRE(status == 0);
}

TEST_CASE("Mangling leaves comments alone", "[fjsl][compose]") {
    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_sky_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    // A comment that mentions a uniform by name is prose, not code. Renaming
    // inside it is harmless to the compiler and baffling to a reader of the
    // generated file.
    const std::string fn =
        "shader_function;\n"
        "in vec3 view_dir;\nout vec3 color;\n"
        "uniform float halo : hint_range(0.0, 4.0, 0.05) = 1.0;\n"
        "// The disc and the halo around it.\n"
        "/* halo again, in a block comment */\n"
        "void main() { color = vec3(halo); }\n";
    const std::string root =
        "shader_type sky;\n"
        "use sun = \"lib/f.fjsl\"(view_dir: VIEW_DIR);\n"
        "void sky() { SKY = sun.color; }\n";

    auto meta = parse_fjsl(root);
    meta.source_text = root;
    auto comp = compose(root, {{"lib/f.fjsl", fn}});
    REQUIRE_FALSE(comp.has_errors());
    const std::string glsl = generate_fragment_shader(meta, tmpl, &comp);

    REQUIRE(glsl.find("// The disc and the halo around it.") != std::string::npos);
    REQUIRE(glsl.find("/* halo again, in a block comment */") != std::string::npos);
    REQUIRE(glsl.find("vec3(sun_halo)") != std::string::npos);  // the code did change
}

TEST_CASE("A composition emits on its own what the generator embeds", "[fjsl][compose]") {
    // A host with a template of its own composes shader functions without a
    // shader type: it asks for the composition's GLSL directly. Given the
    // engine's own way of reading uniforms, that is exactly the text the
    // generator places where the template's globals go.
    const std::string fn =
        "shader_function;\n"
        "in vec3 view_dir;\nout vec3 color;\n"
        "uniform float size : hint_range(0.1, 4.0, 0.01) = 1.8;\n"
        "uniform sampler2D ramp : hint_black;\n"
        "void main() { color = texture(ramp, view_dir.xy).rgb * size; }\n";
    const std::string root =
        "shader_type sky;\n"
        "use sun = \"lib/f.fjsl\"(view_dir: VIEW_DIR);\n"
        "uniform float own : hint_range(0.0, 1.0, 0.1) = 0.5;\n"
        "void sky() { SKY = sun.color * own; }\n";
    auto meta = parse_fjsl(root);
    meta.source_text = root;
    auto comp = compose(root, {{"lib/f.fjsl", fn}});
    REQUIRE_FALSE(comp.has_errors());

    const std::string mat = "materials[push.sky_material_id]";
    const FjslUniformAccess sky_access{
        .param = [&](uint32_t i) { return "custom_params[" + mat + ".param_offset + " + std::to_string(i) + "]"; },
        .texture = [&](uint32_t s) {
            return "textures[nonuniformEXT(" + mat + ".texture_indices[" + std::to_string(s) + "])]";
        },
    };
    const std::string emitted =
        emit_composition(comp, sky_access, meta.custom_param_count, meta.custom_texture_count);
    const std::string generated = generate_fragment_shader(meta, "#version 460\n/*FJSL_USER_GLOBALS*/", &comp);
    CHECK(generated.find(emitted + meta.user_globals) != std::string::npos);
}

TEST_CASE("A host says where a composition's uniforms live", "[fjsl][compose]") {
    const std::string fn =
        "shader_function;\n"
        "in vec2 uv;\nout float value;\n"
        "uniform float scale : hint_range(0.1, 8.0, 0.1) = 2.0;\n"
        "uniform sampler2D grain : hint_black;\n"
        "void main() { value = texture(grain, uv * scale).r; }\n";
    const std::string root =
        "shader_type sky;\n"
        "use noise = \"lib/noise.fjsl\"(uv: VIEW_DIR.xy);\n"
        "void sky() { SKY = vec3(noise.value); }\n";
    auto meta = parse_fjsl(root);
    meta.source_text = root;
    auto comp = compose(root, {{"lib/noise.fjsl", fn}});
    REQUIRE_FALSE(comp.has_errors());

    // A node graph's own layout: one flat array of values, one of samplers.
    const FjslUniformAccess node_access{
        .param = [](uint32_t i) { return "node_params[" + std::to_string(i) + "]"; },
        .texture = [](uint32_t s) { return "node_textures[" + std::to_string(s) + "]"; },
    };
    const std::string glsl = emit_composition(comp, node_access, 0, 0);
    CHECK(glsl.find("#define noise_scale (node_params[0])") != std::string::npos);
    // A sampler's slot counts from after a material's own four textures.
    CHECK(glsl.find("#define noise_grain node_textures[4]") != std::string::npos);
    CHECK(glsl.find("void noise_main(vec2 noise_uv)") != std::string::npos);
    CHECK(glsl.find("materials[") == std::string::npos);

    // And calls what it composed, reading its outputs by their emitted names.
    CHECK(emit_use_calls(meta.uses, comp) == "    noise_main(VIEW_DIR.xy);\n");
    CHECK(rewrite_use_references("SKY = vec3(noise.value);", meta.uses) == "SKY = vec3(noise_value);");
}

TEST_CASE("A composition's parameter block covers what it reads", "[fjsl][compose]") {
    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_sky_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    const std::string fn =
        "shader_function;\n"
        "in vec3 view_dir;\nout vec3 color;\n"
        "uniform float size : hint_range(0.1, 4.0, 0.01) = 1.8;\n"
        "uniform vec3 tint : hint_color = vec3(1.0);\n"
        "void main() { color = tint * size * view_dir.y; }\n";
    const std::string root =
        "shader_type sky;\n"
        "use sun = \"lib/f.fjsl\"(view_dir: VIEW_DIR);\n"
        "uniform vec3 horizon : hint_color = vec3(0.5);\n"
        "void sky() { SKY = horizon + sun.color; }\n";

    auto meta = parse_fjsl(root);
    meta.source_text = root;
    const uint32_t own = meta.custom_param_count;
    REQUIRE(own == 3);  // horizon

    auto comp = compose(root, {{"lib/f.fjsl", fn}});
    REQUIRE_FALSE(comp.has_errors());
    const std::string glsl = generate_fragment_shader(meta, tmpl, &comp);

    // What PipelineCache folds in after generating: the function's uniforms
    // are part of this program and take a run of the same parameter block.
    // A pass sizes the block from custom_param_count, so leaving them out
    // means the shader reads past what was allocated — and the inspector
    // shows none of the dials the shader is reading.
    uint32_t total = own;
    for (const auto& file : comp.files) {
        if (file.use_name.empty()) continue;
        total += file.meta.custom_param_count;
    }
    REQUIRE(total == 7);  // horizon(3) + size(1) + tint(3)

    // The generated GLSL must read inside that, and at the offsets the fold
    // advertises: size at 3, tint at 4..6.
    REQUIRE(glsl.find("param_offset + 3]") != std::string::npos);
    REQUIRE(glsl.find("param_offset + 6]") != std::string::npos);
    REQUIRE(glsl.find("param_offset + 7]") == std::string::npos);
}

