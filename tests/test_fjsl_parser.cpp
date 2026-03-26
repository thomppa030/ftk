#include "renderer/fjsl_parser.hpp"
#include "core/log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using namespace fjell;

static struct LogInit {
    LogInit() { if (!log::core()) log::init(); }
} s_log_init;

// --- shader_type ---

TEST_CASE("Parse shader_type spatial", "[fjsl]") {
    auto meta = parse_fjsl("shader_type spatial;\nvoid fragment() {}");
    REQUIRE(meta.shader_type == "spatial");
}

TEST_CASE("Missing shader_type throws", "[fjsl]") {
    REQUIRE_THROWS(parse_fjsl("void fragment() {}"));
}

TEST_CASE("Unsupported shader_type throws", "[fjsl]") {
    REQUIRE_THROWS(parse_fjsl("shader_type canvas;\nvoid fragment() {}"));
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
    REQUIRE(meta.fragment_body.find("ALBEDO") != std::string::npos);
    REQUIRE(meta.fragment_body.find("mix") != std::string::npos);
}

TEST_CASE("Extract vertex body", "[fjsl]") {
    auto meta = parse_fjsl(R"(
        shader_type spatial;
        void vertex() {
            VERTEX.y += sin(TIME);
        }
        void fragment() {}
    )");
    REQUIRE(meta.vertex_body.find("VERTEX") != std::string::npos);
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
    REQUIRE(meta.fragment_body.find("if") != std::string::npos);
    REQUIRE(meta.fragment_body.find("ALBEDO") != std::string::npos);
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
    REQUIRE(meta.vertex_body.find("VERTEX") != std::string::npos);
    REQUIRE(meta.fragment_body.find("ALBEDO") != std::string::npos);
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
    REQUIRE(meta.fragment_body.find("ALBEDO") != std::string::npos);
    REQUIRE(meta.fragment_body.find("for") != std::string::npos);
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
