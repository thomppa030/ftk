#include "renderer/resources/fjsl_parser.hpp"
#include "ftk/platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using namespace fjell;

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

TEST_CASE("A missing or an unknown shader_type declares nothing", "[fjsl]") {
    REQUIRE_FALSE(declared_shader_type("void fragment() {}\n").has_value());
    REQUIRE_FALSE(declared_shader_type("shader_type particle;").has_value());
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
        "/*FJSL_USER_GLOBALS*/\n"
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
    std::string tmpl = "/*FJSL_USER_GLOBALS*/\nvoid main() {\n/*FJSL_USER_CODE*/\n}\n";
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
    auto frag = generate_fragment_shader(meta, "/*FJSL_USER_GLOBALS*/\n/*FJSL_USER_CODE*/\n");
    REQUIRE(frag.find("int fjell_uniform_mode() { return int(round(custom_params["
                      "materials[draws[frag_draw_index].material_id].param_offset + 2])); }")
            != std::string::npos);
    REQUIRE(frag.find("vec2 fjell_uniform_offset() { return vec2(custom_params[") != std::string::npos);
    auto vert = generate_vertex_shader(meta, "/*FJSL_VERTEX_GLOBALS*/\n/*FJSL_VERTEX_USER_CODE*/\n");
    REQUIRE(vert.find("int fjell_uniform_mode() { return int(round(custom_params[mat.param_offset + 2])); }")
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

// --- Comments are not code ---
//
// A hook named in a comment once erased everything from that line to the next
// closing brace, so the helpers after it went missing from the globals with no
// word said (a comment in sea.fjsl did this for months).

TEST_CASE("A comment naming a hook deletes nothing", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        // The tint seeds ALBEDO before fragment() runs; vertex() and light()
        // are left to the template.
        vec3 tinted(vec3 c) { return c * 0.5; }
        float softened(float x) { return x * x; }
        void fragment() {
            ALBEDO = tinted(ALBEDO);
        }
    )");
    REQUIRE_FALSE(meta.has_errors());
    CHECK(meta.user_globals.find("vec3 tinted(") != std::string::npos);
    CHECK(meta.user_globals.find("float softened(") != std::string::npos);
    CHECK(meta.fragment_body.code.find("ALBEDO = tinted(ALBEDO);") != std::string::npos);
    CHECK(meta.vertex_body.empty());
    CHECK(meta.light_body.empty());
}

TEST_CASE("A brace in a comment does not end a hook's body", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        void fragment() {
            ROUGHNESS = 0.4;  // keep } out of the way
            /* { } */
            METALLIC = 1.0;
        }
    )");
    REQUIRE_FALSE(meta.has_errors());
    CHECK(meta.fragment_body.code.find("METALLIC = 1.0;") != std::string::npos);
    // The comments themselves stay in the body.
    CHECK(meta.fragment_body.code.find("keep } out of the way") != std::string::npos);
}

TEST_CASE("A helper named like a hook is not the hook", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        void fragment_tint(inout vec3 c) { c *= 0.5; }
        void fragment() {
            fragment_tint(ALBEDO);
        }
    )");
    REQUIRE_FALSE(meta.has_errors());
    CHECK(meta.fragment_body.code.find("fragment_tint(ALBEDO);") != std::string::npos);
    CHECK(meta.user_globals.find("void fragment_tint(") != std::string::npos);
}

TEST_CASE("A refused hook named only in a comment is not refused", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type ui;
        // There is no void vertex() for a ui quad; it paints in fragment().
        void fragment() {
            ALBEDO = vec3(1.0);
        }
    )");
    CHECK_FALSE(meta.has_errors());
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
/*FJSL_USER_GLOBALS*/
void main() {
/*FJSL_USER_CODE*/
}
)";

    auto result = generate_fragment_shader(meta, tmpl);
    // The uniform is read from the SSBO where the code names it.
    REQUIRE(result.find("float fjell_uniform_strength() { return custom_params[") != std::string::npos);
    // User code should be injected
    REQUIRE(result.find("ALBEDO *= fjell_uniform_strength()") != std::string::npos);
    // Placeholders should be gone
    REQUIRE(result.find("/*FJSL_USER_GLOBALS*/") == std::string::npos);
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
/*FJSL_VERTEX_GLOBALS*/
void fjell_hook_vertex() {
/*FJSL_VERTEX_USER_CODE*/
}
)";

    auto result = generate_vertex_shader(meta, tmpl);
    // A uniform is read through a function at file scope, as on the fragment side.
    REQUIRE(result.find("float fjell_uniform_wave_height() { return custom_params[mat.param_offset + 0]; }")
            != std::string::npos);
    REQUIRE(result.find("sin(VERTEX.x) * fjell_uniform_wave_height()") != std::string::npos);
    REQUIRE(result.find("VERTEX.y") != std::string::npos);
    REQUIRE(result.find("/*FJSL_VERTEX_GLOBALS*/") == std::string::npos);
    REQUIRE(result.find("/*FJSL_VERTEX_USER_CODE*/") == std::string::npos);
}

// A file's helpers go to the stages whose hooks reach them. glslang checks
// every function it is given, so a helper that reads a fragment built-in
// would fail a vertex stage that never calls it.
TEST_CASE("Each stage gets the helpers its hooks reach", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform float amount = 1.0;
        const float LIFT = 0.25;
        struct Wave { float height; };
        #define SCALE 2.0

        // Moves a vertex; the vertex hook calls it.
        float sway(float x) { return wiggle(x) * amount; }
        float wiggle(float x) { return sin(x * SCALE) * LIFT; }

        // Reads a fragment built-in; the fragment hook calls it.
        vec3 screen_tint() { return vec3(SCREEN_UV, 0.0); }

        // Called by nothing.
        float unused_helper(float x) { return VERTEX.x + x; }

        void vertex() {
            VERTEX.y += sway(VERTEX.x);
        }
        void fragment() {
            ALBEDO *= screen_tint();
        }
    )");
    REQUIRE_FALSE(meta.has_errors());

    const std::string vert = generate_vertex_shader(meta, "/*FJSL_VERTEX_GLOBALS*/\n/*FJSL_VERTEX_USER_CODE*/\n");
    CHECK(vert.find("float sway(") != std::string::npos);
    CHECK(vert.find("float wiggle(") != std::string::npos);  // reached through sway()
    CHECK(vert.find("const float LIFT") != std::string::npos);
    CHECK(vert.find("struct Wave") != std::string::npos);
    CHECK(vert.find("#define SCALE") != std::string::npos);
    CHECK(vert.find("screen_tint") == std::string::npos);
    CHECK(vert.find("unused_helper") == std::string::npos);
    // A helper's comment leaves with it.
    CHECK(vert.find("Reads a fragment built-in") == std::string::npos);

    const std::string frag = generate_fragment_shader(
        meta, "/*FJSL_USER_GLOBALS*/\n/*FJSL_USER_CODE*/\n");
    CHECK(frag.find("vec3 screen_tint(") != std::string::npos);
    CHECK(frag.find("const float LIFT") != std::string::npos);
    CHECK(frag.find("float sway(") == std::string::npos);
    CHECK(frag.find("unused_helper") == std::string::npos);
}

TEST_CASE("A macro or a constant calling a helper keeps it", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        float ramp(float x) { return clamp(x, 0.0, 1.0); }
        #define RAMP(x) ramp(x)
        void fragment() {
            ROUGHNESS = RAMP(0.5);
        }
    )");
    REQUIRE_FALSE(meta.has_errors());
    const std::string frag = generate_fragment_shader(meta, "/*FJSL_USER_GLOBALS*/\n/*FJSL_USER_CODE*/\n");
    CHECK(frag.find("float ramp(") != std::string::npos);
}

TEST_CASE("A sampler2D uniform is a texture handle from the row", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        uniform sampler2D detail_tex;
        void fragment() {}
    )");

    std::string tmpl = "/*FJSL_USER_GLOBALS*/\n/*FJSL_USER_CODE*/\n";
    auto result = generate_fragment_shader(meta, tmpl);
    REQUIRE(result.find("FjellTexture fjell_uniform_detail_tex() { return FjellTexture("
                        "materials[draws[frag_draw_index].material_id].texture_indices[4]); }")
            != std::string::npos);
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
                     ftk::platform::executable_path().parent_path()}) {
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
    int status = ftk::platform::run_command(cmd + " 2>&1", output);
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

TEST_CASE("Every fog .fjsl generates both compute variants glslc accepts", "[fjsl][glslc]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");

    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_fog_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-fog-compile-test";
    std::filesystem::create_directories(out_dir);

    int checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(shader_dir)) {
        if (entry.path().extension() != ".fjsl") continue;
        auto name = entry.path().stem().string();
        INFO("shader: " << name);

        auto meta = parse_fjsl(read_text(entry.path()));
        if (meta.shader_type != ShaderType::Fog) continue;

        // As the pipeline cache compiles it: the medium variant, then the
        // same source once more as the light variant.
        auto source = out_dir / (name + ".comp");
        std::ofstream(source) << generate_fragment_shader(meta, tmpl);
        for (const std::string variant : {"", "-DFJELL_FOG_LIGHT "}) {
            INFO("variant: " << (variant.empty() ? "medium" : "light"));
            auto spv = out_dir / (name + ".spv");
            auto [status, output] = run("glslc --target-env=vulkan1.3 -fshader-stage=compute " + variant
                                        + "-I " + quoted(shader_dir / "include") + " "
                                        + quoted(source) + " -o " + quoted(spv));
            INFO(output);
            REQUIRE(status == 0);
        }
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

// A file's helpers reach vertex() in every vertex and mesh template, reading
// the parameters by name, while a helper only fragment() calls, reading a
// fragment built-in, stays out of them; and the other way round for the
// fragment template.
TEST_CASE("Helpers reach vertex() in every vertex template, and stay in their stage", "[fjsl][glslc]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");

    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());

    const std::pair<const char*, const char*> templates[] = {
        {"fjell_mesh_template.glsl", "mesh"},
        {"fjell_vertex_template.glsl", "vert"},
        {"fjell_depth_mesh_template.glsl", "mesh"},
        {"fjell_depth_vertex_template.glsl", "vert"},
        {"fjell_shadow_vertex_template.glsl", "vert"},
        {"fjell_outline_vertex_template.glsl", "vert"},
    };
    auto meta = parse_fjsl("shader_type spatial;\n"
                           "uniform float strength : hint_range(0.0, 1.0) = 0.3;\n"
                           "uniform vec3 tint : hint_color = vec3(1.0);\n"
                           "const float PHASE = 1.7;\n"
                           "vec3 sway(vec3 p, float t) {\n"
                           "    return vec3(sin(p.x + t * PHASE), 0.0, cos(p.z + t)) * strength;\n"
                           "}\n"
                           "vec3 screen_tint() { return tint * vec3(SCREEN_UV, 1.0); }\n"
                           "void vertex() {\n    VERTEX += sway(WORLD_VERTEX, TIME) * UV.y;\n}\n"
                           "void fragment() {\n    ALBEDO *= screen_tint();\n}\n");
    REQUIRE_FALSE(meta.has_errors());

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-vertex-helpers-test";
    std::filesystem::create_directories(out_dir);
    auto compile = [&](const std::string& name, const std::string& stage, const std::string& glsl) {
        auto src = out_dir / name;
        std::ofstream(src) << glsl;
        auto [status, output] = run("glslc --target-env=vulkan1.3 -fshader-stage=" + stage + " -I "
                                    + quoted(shader_dir / "include") + " " + quoted(src) + " -o "
                                    + quoted(out_dir / (name + ".spv")));
        INFO(output);
        REQUIRE(status == 0);
    };

    for (const auto& [file, stage] : templates) {
        INFO("template: " << file);
        auto tmpl = read_text(shader_dir / "include" / file);
        REQUIRE_FALSE(tmpl.empty());
        const std::string glsl = generate_vertex_shader(meta, tmpl);
        CHECK(glsl.find("vec3 sway(") != std::string::npos);
        CHECK(glsl.find("screen_tint") == std::string::npos);
        compile(file, stage, glsl);
    }

    auto frag_tmpl = read_text(shader_dir / "include" / "fjell_spatial_template.glsl");
    REQUIRE_FALSE(frag_tmpl.empty());
    const std::string frag = generate_fragment_shader(meta, frag_tmpl);
    CHECK(frag.find("vec3 screen_tint(") != std::string::npos);
    CHECK(frag.find("vec3 sway(") == std::string::npos);
    compile("helpers.frag", "frag", frag);
}

// A uniform's name means the uniform only where GLSL's scoping says so. A
// struct field, a parameter or a local of the same name is its own, in every
// stage, and the uniform is still what the hooks and helpers read elsewhere.
TEST_CASE("A field, a parameter or a local may share a uniform's name", "[fjsl][glslc]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");
    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());

    auto meta = parse_fjsl("shader_type spatial;\n"
                           "uniform float halo : hint_range(0.0, 1.0, 0.01) = 0.5;\n"
                           "uniform vec3 tint : hint_color = vec3(1.0);\n"
                           "struct Glow { float halo; vec3 tint; };\n"
                           "float doubled(float halo) { return halo * 2.0; }\n"
                           "float lift() { return halo * 0.1; }\n"
                           "vec3 glow(Glow look) { return look.tint * doubled(look.halo); }\n"
                           "void vertex() {\n    float tint = lift();\n    VERTEX.y += tint;\n}\n"
                           "void fragment() {\n"
                           "    Glow look;\n    look.halo = halo;\n    look.tint = tint;\n"
                           "    ALBEDO = glow(look);\n}\n");
    REQUIRE_FALSE(meta.has_errors());

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-uniform-scope-test";
    std::filesystem::create_directories(out_dir);
    auto compile = [&](const std::string& name, const std::string& stage, const std::string& glsl) {
        auto src = out_dir / name;
        std::ofstream(src) << glsl;
        auto [status, output] = run("glslc --target-env=vulkan1.3 -fshader-stage=" + stage + " -I "
                                    + quoted(shader_dir / "include") + " " + quoted(src) + " -o "
                                    + quoted(out_dir / (name + ".spv")));
        INFO(output);
        REQUIRE(status == 0);
    };

    const std::pair<const char*, const char*> vertex_templates[] = {
        {"fjell_mesh_template.glsl", "mesh"},
        {"fjell_vertex_template.glsl", "vert"},
        {"fjell_depth_mesh_template.glsl", "mesh"},
        {"fjell_depth_vertex_template.glsl", "vert"},
        {"fjell_shadow_vertex_template.glsl", "vert"},
        {"fjell_outline_vertex_template.glsl", "vert"},
    };
    for (const auto& [file, stage] : vertex_templates) {
        INFO("template: " << file);
        auto tmpl = read_text(shader_dir / "include" / file);
        REQUIRE_FALSE(tmpl.empty());
        compile(file, stage, generate_vertex_shader(meta, tmpl));
    }
    auto frag_tmpl = read_text(shader_dir / "include" / "fjell_spatial_template.glsl");
    REQUIRE_FALSE(frag_tmpl.empty());
    compile("scope.frag", "frag", generate_fragment_shader(meta, frag_tmpl));
}

// The same for what a shader includes, which it did not write and cannot
// rename: the painted sun's struct has fields a sky's own uniforms are
// naturally called.
TEST_CASE("An include may declare what a uniform is called", "[fjsl][glslc]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");
    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_sky_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    auto meta = parse_fjsl("shader_type sky;\n"
                           "#include \"sky/painted_sun.glsl\"\n"
                           "uniform float size : hint_range(0.1, 8.0, 0.01) = 1.8;\n"
                           "uniform float halo : hint_range(0.0, 4.0, 0.05) = 1.0;\n"
                           "void sky() {\n"
                           "    PaintedSun look;\n"
                           "    look.size = size;\n    look.halo = halo;\n"
                           "    look.halo_size = 8.0;\n    look.halo_core = 1.5;\n"
                           "    look.halo_spread = 0.45;\n    look.halo_stretch = 1.0;\n"
                           "    look.stylize = 0.0;\n    look.form_scale = 1.2;\n"
                           "    look.tear = 0.06;\n    look.corona = 0.22;\n"
                           "    look.core = 0.62;\n    look.hot = 0.0;\n    look.boil = 0.06;\n"
                           "    look.rim_color = vec3(1.0);\n    look.body_color = vec3(1.0);\n"
                           "    look.hot_color = vec3(1.0);\n"
                           "    SKY = painted_sun(look, VIEW_DIR, SUN_DIR, SUN_COLOR, TIME, 1.0, 1.0);\n"
                           "}\n");
    REQUIRE_FALSE(meta.has_errors());

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-uniform-include-test";
    std::filesystem::create_directories(out_dir);
    auto src = out_dir / "include_scope.frag";
    std::ofstream(src) << generate_fragment_shader(meta, tmpl);
    const std::string includes = "-I " + quoted(shader_dir / "include") + " ";
    auto [drawn, drawn_output] = run("glslc --target-env=vulkan1.3 " + includes + quoted(src)
                                     + " -o " + quoted(out_dir / "include_scope.spv"));
    INFO(drawn_output);
    REQUIRE(drawn == 0);
    auto [baked, baked_output] = run("glslc --target-env=vulkan1.3 -fshader-stage=compute "
                                     "-DFJELL_SKY_CUBEMAP " + includes + quoted(src) + " -o "
                                     + quoted(out_dir / "include_scope.cubemap.spv"));
    INFO(baked_output);
    REQUIRE(baked == 0);
}

// Each place a name could mean the uniform, and what it means there.
TEST_CASE("A uniform is read where the code names it, by GLSL's scoping", "[fjsl]") {
    auto meta = parse_fjsl("shader_type spatial;\n"
                           "uniform float halo = 0.5;\n"
                           "uniform vec3 tint : hint_color = vec3(1.0);\n"
                           "struct Glow { float halo; vec3 tint; };\n"
                           "float doubled(float halo) { return halo * 2.0; }\n"
                           "float lift() { return halo * 0.1; }\n"
                           "#define BRIGHT (halo * 4.0)\n"
                           "void fragment() {\n"
                           "    Glow look;\n"
                           "    look.halo = halo; // halo, in a comment\n"
                           "    for (int tint = 0; tint < 2; ++tint) { ALBEDO.r += float(tint); }\n"
                           "    { vec3 tint = vec3(0.0); ALBEDO += tint; }\n"
                           "    float a = 1.0, halo = lift();\n"
                           "    ALBEDO *= tint * doubled(look.halo) * BRIGHT * halo;\n"
                           "}\n");
    REQUIRE_FALSE(meta.has_errors());
    const std::string glsl = generate_fragment_shader(
        meta, "/*FJSL_USER_GLOBALS*/\nvoid main() {\n/*FJSL_USER_CODE*/\n}\n");
    INFO(glsl);
    // A struct field, a parameter and a member access are their own.
    CHECK(glsl.find("struct Glow { float halo; vec3 tint; };") != std::string::npos);
    CHECK(glsl.find("float doubled(float halo) { return halo * 2.0; }") != std::string::npos);
    // A helper and a macro read the uniform, and so does a hook.
    CHECK(glsl.find("return fjell_uniform_halo() * 0.1;") != std::string::npos);
    CHECK(glsl.find("#define BRIGHT (fjell_uniform_halo() * 4.0)") != std::string::npos);
    CHECK(glsl.find("look.halo = fjell_uniform_halo(); // halo, in a comment") != std::string::npos);
    // A for's variable and a block's local hold to the end of their scope.
    CHECK(glsl.find("for (int tint = 0; tint < 2; ++tint) { ALBEDO.r += float(tint); }")
          != std::string::npos);
    CHECK(glsl.find("{ vec3 tint = vec3(0.0); ALBEDO += tint; }") != std::string::npos);
    // After them the name is the uniform again, until a local takes it for
    // the rest of the body, the second of two declared on one line.
    CHECK(glsl.find("ALBEDO *= fjell_uniform_tint() * doubled(look.halo) * BRIGHT * halo;")
          != std::string::npos);
}

// A uniform is never called, so a name followed by `(` is a function. A post
// shader may have a `vignette` dial and call the vignette() effect with it.
TEST_CASE("A uniform may share its name with a function", "[fjsl][glslc][post]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");
    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_post_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    auto meta = parse_fjsl("shader_type post;\n"
                           "uniform float vignette : hint_range(0.0, 1.0, 0.01) = 0.6;\n"
                           "void color() { COLOR *= vignette(SCREEN_UV, vignette, 0.5); }\n");
    REQUIRE_FALSE(meta.has_errors());
    const std::string glsl = generate_fragment_shader(meta, tmpl);
    CHECK(glsl.find("vignette(SCREEN_UV, fjell_uniform_vignette(), 0.5)") != std::string::npos);

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-uniform-function-test";
    std::filesystem::create_directories(out_dir);
    auto src = out_dir / "shared_name.frag";
    std::ofstream(src) << glsl;
    auto [status, output] = run("glslc --target-env=vulkan1.3 -I " + quoted(shader_dir / "include")
                                + " " + quoted(src) + " -o " + quoted(out_dir / "shared_name.spv"));
    INFO(output);
    REQUIRE(status == 0);
}

TEST_CASE("Writing a uniform is refused where it is written", "[fjsl]") {
    const std::string head = "shader_type spatial;\n"
                             "uniform float halo = 0.5;\n"
                             "uniform vec3 tint : hint_color = vec3(1.0);\n";
    for (const std::string write : {"tint = vec3(0.0);", "tint.r = 0.5;", "halo += 1.0;",
                                    "halo++;", "--halo;"}) {
        INFO("write: " << write);
        auto meta = parse_fjsl(head + "void fragment() {\n    " + write + "\n}\n");
        REQUIRE(meta.has_errors());
        const auto& d = meta.diagnostics.front();
        CHECK(d.line == 5);
        CHECK(d.message.find("is a uniform") != std::string::npos);
    }
    // A local of the same name, a member, and a comparison write nothing of
    // the uniform's.
    auto meta = parse_fjsl(head + "struct Glow { float halo; };\n"
                                  "void fragment() {\n"
                                  "    Glow look;\n    look.halo = 1.0;\n"
                                  "    if (halo == 1.0) { ALBEDO = tint; }\n"
                                  "    float halo = 2.0;\n    halo += 1.0;\n"
                                  "}\n");
    CHECK_FALSE(meta.has_errors());
}

TEST_CASE("A uniform's name cannot be declared again at file scope", "[fjsl]") {
    auto meta = parse_fjsl("shader_type spatial;\n"
                           "uniform float halo = 0.5;\n"
                           "const float halo = 1.0;\n"
                           "void fragment() {}\n");
    REQUIRE(meta.has_errors());
    CHECK(meta.diagnostics.front().line == 3);
    CHECK(meta.diagnostics.front().message.find("already a uniform") != std::string::npos);

    // A parameter, a field and a function are not file-scope declarations of
    // the name.
    auto fine = parse_fjsl("shader_type spatial;\n"
                           "uniform float halo = 0.5;\n"
                           "struct Glow { float halo; };\n"
                           "float doubled(float halo) { return halo * 2.0; }\n"
                           "void fragment() {}\n");
    CHECK_FALSE(fine.has_errors());
}

// --- A `return` in a hook ends the hook, not the shader ---
//
// An author's hook is a function, as in Godot: returning early keeps what it
// has set and lets the template shade from it. Pasted into main() instead, a
// `return` ends the whole shader before it writes its outputs, and the pixel
// keeps undefined values, which bloom spreads into black boxes. So every
// hook's body has to land in a function of its own that main() calls, and
// has to compile there.

TEST_CASE("A return in a hook ends the hook, and the template goes on", "[fjsl][glslc]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");

    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());

    struct Hook {
        const char* hook;       // the function the body lands in
        const char* statement;  // the early return, as written in the body
    };
    struct Case {
        const char* name;
        const char* tmpl;
        const char* source;
        std::vector<Hook> hooks;
    };
    const Case cases[] = {
        {"spatial", "fjell_spatial_template.glsl",
         "shader_type spatial;\n"
         "void fragment() {\n    ALBEDO = vec3(0.01);\n    if (UV.x > 0.5) return;\n    ALBEDO = vec3(1.0);\n}\n",
         {{"fjell_hook_fragment", "if (UV.x > 0.5) return;"}}},
        {"spatial_unlit", "fjell_spatial_template.glsl",
         "shader_type spatial;\nrender_mode unlit;\n"
         "void fragment() {\n    ALBEDO = vec3(0.01);\n    if (UV.x > 0.5) return;\n    EMISSION = vec3(1.0);\n}\n",
         {{"fjell_hook_fragment", "if (UV.x > 0.5) return;"}}},
        {"terrain", "fjell_terrain_template.glsl",
         "shader_type terrain;\n"
         "void layers() {\n    if (SLOPE > 30.0) return;\n    TRIPLANAR = 0.0;\n}\n"
         "void fragment() {\n    if (WORLD_POS.y > 10.0) return;\n    ROUGHNESS = 1.0;\n}\n",
         {{"fjell_hook_layers", "if (SLOPE > 30.0) return;"},
          {"fjell_hook_fragment", "if (WORLD_POS.y > 10.0) return;"}}},
        {"water", "fjell_water_template.glsl",
         "shader_type water;\n"
         "void fragment() {\n    if (BELOW > 0.5) return;\n    ALBEDO = vec3(0.1);\n}\n",
         {{"fjell_hook_fragment", "if (BELOW > 0.5) return;"}}},
        {"ui", "fjell_ui_template.glsl",
         "shader_type ui;\n"
         "void fragment() {\n    if (HOVER > 0.5) return;\n    ALBEDO = vec3(1.0);\n}\n",
         {{"fjell_hook_fragment", "if (HOVER > 0.5) return;"}}},
        {"sky", "fjell_sky_template.glsl",
         "shader_type sky;\n"
         "void sky() {\n    SKY = vec3(0.2);\n    if (VIEW_DIR.y < 0.0) return;\n    SKY = vec3(1.0);\n}\n",
         {{"fjell_hook_sky", "if (VIEW_DIR.y < 0.0) return;"}}},
        {"post", "fjell_post_template.glsl",
         "shader_type post;\n"
         "void color() {\n    if (DEPTH > 100.0) return;\n    COLOR *= 0.5;\n}\n",
         {{"fjell_hook_color", "if (DEPTH > 100.0) return;"}}},
    };

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-return-test";
    std::filesystem::create_directories(out_dir);

    for (const auto& c : cases) {
        INFO("case: " << c.name);
        auto tmpl = read_text(shader_dir / "include" / c.tmpl);
        REQUIRE_FALSE(tmpl.empty());
        auto meta = parse_fjsl(c.source);
        REQUIRE_FALSE(meta.has_errors());
        const std::string glsl = generate_fragment_shader(meta, tmpl);

        const auto main_at = glsl.find("void main()");
        REQUIRE(main_at != std::string::npos);
        for (const auto& h : c.hooks) {
            INFO("hook: " << h.hook);
            const auto hook_at = glsl.find(std::string("void ") + h.hook + "()");
            const auto return_at = glsl.find(h.statement);
            // Called from the template past the hook, from main() or the
            // function main() calls (fjell_sky, fjell_post).
            const auto call_at = glsl.find(std::string(h.hook) + "();", return_at);
            REQUIRE(hook_at != std::string::npos);
            REQUIRE(return_at != std::string::npos);
            REQUIRE(call_at != std::string::npos);
            REQUIRE(hook_at < return_at);
            REQUIRE(return_at < main_at);
        }

        auto frag = out_dir / (std::string(c.name) + ".frag");
        std::ofstream(frag) << glsl;
        auto [status, output] = run("glslc --target-env=vulkan1.3 -I " + quoted(shader_dir / "include") + " "
                                    + quoted(frag) + " -o " + quoted(out_dir / (std::string(c.name) + ".spv")));
        INFO(output);
        REQUIRE(status == 0);
    }
}

TEST_CASE("A return in vertex() ends the hook in every vertex template", "[fjsl][glslc]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");

    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());

    const std::pair<const char*, const char*> templates[] = {
        {"fjell_mesh_template.glsl", "mesh"},
        {"fjell_vertex_template.glsl", "vert"},
        {"fjell_depth_mesh_template.glsl", "mesh"},
        {"fjell_depth_vertex_template.glsl", "vert"},
        {"fjell_shadow_vertex_template.glsl", "vert"},
        {"fjell_outline_vertex_template.glsl", "vert"},
    };
    auto meta = parse_fjsl("shader_type spatial;\n"
                           "uniform float lift = 0.5;\n"
                           "void vertex() {\n    if (UV.x > 0.5) return;\n    VERTEX.y += lift;\n}\n"
                           "void fragment() {}\n");
    REQUIRE_FALSE(meta.has_errors());

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-vertex-return-test";
    std::filesystem::create_directories(out_dir);

    for (const auto& [file, stage] : templates) {
        INFO("template: " << file);
        auto tmpl = read_text(shader_dir / "include" / file);
        REQUIRE_FALSE(tmpl.empty());
        const std::string glsl = generate_vertex_shader(meta, tmpl);

        const auto hook_at = glsl.find("void fjell_hook_vertex()");
        const auto return_at = glsl.find("if (UV.x > 0.5) return;");
        const auto main_at = glsl.find("void main()");
        REQUIRE(hook_at != std::string::npos);
        REQUIRE(hook_at < return_at);
        REQUIRE(return_at < main_at);
        REQUIRE(glsl.find("fjell_hook_vertex();", main_at) != std::string::npos);

        auto src = out_dir / file;
        std::ofstream(src) << glsl;
        auto [status, output] = run("glslc --target-env=vulkan1.3 -fshader-stage=" + std::string(stage) + " -I "
                                    + quoted(shader_dir / "include") + " " + quoted(src) + " -o "
                                    + quoted(out_dir / (std::string(file) + ".spv")));
        INFO(output);
        REQUIRE(status == 0);
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
    std::string tmpl = "/*FJSL_USER_GLOBALS*/\n/*FJSL_USER_CODE*/\n";
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
    const std::string tmpl = "/*FJSL_USER_GLOBALS*/\nvoid f() {\n/*FJSL_USER_CODE*/\n}\n"
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
    auto glsl = generate_fragment_shader(meta, "/*FJSL_USER_GLOBALS*/\n/*FJSL_USER_CODE*/\n");
    // The values differ per viewport, so they come from the pass's buffer at
    // the pushed base, never from the material row's slice of the params.
    REQUIRE(glsl.find("vec3 fjell_uniform_tint() { return vec3(post_params[push.param_base + 0], "
                      "post_params[push.param_base + 1], post_params[push.param_base + 2]); }")
            != std::string::npos);
    REQUIRE(glsl.find("int fjell_uniform_mode() { return int(round(post_params[push.param_base + 3])); }")
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
    std::string tmpl = "/*FJSL_USER_GLOBALS*/\n/*FJSL_LAYERS_USER_CODE*/\n/*FJSL_USER_CODE*/\n";
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

// --- Shared code ---
//
// Code is shared as plain GLSL: a .glsl file under shaders/include, included
// and called. What a file from the days of composition wrote is reported
// where it is, saying where its code goes now.

TEST_CASE("A leftover shader_function or use says where shared code goes", "[fjsl]") {
    auto function = parse_fjsl("// a shared sun\nshader_function;\nin vec3 dir;\nout vec3 color;\n"
                               "void main() { color = dir; }\n");
    REQUIRE(function.has_errors());
    CHECK(function.diagnostics.size() == 1);
    CHECK(function.diagnostics.front().line == 2);
    CHECK(function.diagnostics.front().message.find("#include") != std::string::npos);

    auto use = parse_fjsl("shader_type sky;\n"
                          "use sun = \"lib/sun.fjsl\"(dir: VIEW_DIR, size: mix(1.0, 2.0, 0.5));\n"
                          "void sky() { SKY = sun.color; }\n");
    REQUIRE(use.has_errors());
    CHECK(use.diagnostics.size() == 1);
    CHECK(use.diagnostics.front().line == 2);
    CHECK(use.diagnostics.front().message.find("lib/sun.fjsl") != std::string::npos);
    CHECK(use.diagnostics.front().message.find("#include") != std::string::npos);
}

// The engine's own skies, with the sky/ includes they draw their bodies with.
// A sky is compiled twice — once as the fragment shader that draws it and
// once as the compute shader that fills the lighting cubemap — and a function
// that is fine in one can be illegal in the other: a screen-space derivative,
// for one, has no meaning in compute.
TEST_CASE("Every shipped sky compiles both ways", "[fjsl][glslc]") {
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

        auto frag = out_dir / (name + ".frag");
        std::ofstream(frag) << generate_fragment_shader(meta, tmpl);

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

TEST_CASE("An author's function takes a material texture as a sampler2D", "[fjsl]") {
    if (run("glslc --version").first != 0) SKIP("glslc not available");
    auto shader_dir = find_shader_dir();
    REQUIRE_FALSE(shader_dir.empty());
    auto tmpl = read_text(shader_dir / "include" / "fjell_sky_template.glsl");
    REQUIRE_FALSE(tmpl.empty());

    // GLSL passes no texture and sampler combined to a function; a material
    // texture travels as a handle, and the author's own sampler2D parameter
    // has to take it.
    const std::string root =
        "shader_type sky;\n"
        "uniform sampler2D ramp : hint_black;\n"
        "vec3 ramp_at(sampler2D t, float x) { return textureLod(t, vec2(x, 0.5), 0.0).rgb; }\n"
        "void sky() { SKY = ramp_at(ramp, VIEW_DIR.y) * float(textureSize(ramp, 0).x); }\n";
    auto meta = parse_fjsl(root);
    meta.source_text = root;
    REQUIRE_FALSE(meta.has_errors());
    const std::string glsl = generate_fragment_shader(meta, tmpl);

    auto out_dir = std::filesystem::temp_directory_path() / "fjell-fjsl-handle-test";
    std::filesystem::create_directories(out_dir);
    auto frag = out_dir / "handle.frag";
    std::ofstream(frag) << glsl;
    auto [status, output] = run("glslc --target-env=vulkan1.3 -I "
                                + quoted(shader_dir / "include") + " "
                                + quoted(frag) + " -o " + quoted(out_dir / "h.spv"));
    INFO(output);
    REQUIRE(status == 0);
}
