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
        if (meta.shader_type != "spatial") continue;  // sky shaders use their own template

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
