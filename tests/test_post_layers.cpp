#include "renderer/post_layers.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <deque>

using namespace fjell;
using Catch::Approx;

namespace {

constexpr uint32_t DEFAULT_SHADER = 7;
constexpr uint32_t GRADE_SHADER = 9;

PostLook look_of(uint32_t shader, uint32_t row = 0) {
    PostLook look;
    look.pipeline_id = shader;
    look.material_id = row;
    return look;
}

NamedValue scalar(const char* name, float value) {
    return NamedValue{.name = name, .count = 1, .values = {value, 0.0f, 0.0f, 0.0f}};
}

float value_of(const PostLayer& layer, const char* name) {
    const NamedValue* v = find_named(layer.values, name);
    REQUIRE(v != nullptr);
    return v->values[0];
}

PostLayer engine_default() {
    return {look_of(DEFAULT_SHADER), {scalar("bloom_intensity", 0.3f), scalar("operator", 3.0f)}, 1.0f};
}

// Volumes own their resolved state, and a candidate points at it, as the pass
// builds them from the frame's environment.
struct Volumes {
    std::deque<PostVolume> volumes;
    std::vector<PostCandidate> candidates;

    PostVolume& box(glm::vec3 centre, float half_size, PostLook look, int priority = 0) {
        auto& v = volumes.emplace_back();
        v.transform = glm::scale(glm::translate(glm::mat4(1.0f), centre), glm::vec3(half_size));
        v.priority = priority;
        candidates.push_back({&v, look});
        return v;
    }
    PostVolume& infinite(PostLook look, int priority = 0) {
        auto& v = box(glm::vec3(0.0f), 1.0f, look, priority);
        v.infinite = true;
        return v;
    }
    std::vector<PostLayer> plan(glm::vec3 camera) const {
        return plan_post_layers(engine_default(), candidates, camera);
    }
};

} // namespace

TEST_CASE("With no volume the engine default is the one layer", "[post]") {
    Volumes scene;
    const auto layers = scene.plan(glm::vec3(0.0f));
    REQUIRE(layers.size() == 1);
    REQUIRE(layers[0].look.pipeline_id == DEFAULT_SHADER);
    REQUIRE(layers[0].weight == 1.0f);
    REQUIRE(value_of(layers[0], "bloom_intensity") == Approx(0.3f));
}

TEST_CASE("A volume of the same shader blends its values across its edge without a step", "[post]") {
    Volumes scene;
    auto& v = scene.box(glm::vec3(0.0f), 10.0f, look_of(DEFAULT_SHADER, 4));
    v.edge_falloff = 0.2f;
    v.uniforms = {scalar("bloom_intensity", 1.0f), scalar("operator", 2.0f)};

    // Outside: untouched. Inside: the volume's own. One draw either way.
    auto outside = scene.plan({20.0f, 0.0f, 0.0f});
    REQUIRE(outside.size() == 1);
    REQUIRE(value_of(outside[0], "bloom_intensity") == Approx(0.3f));
    auto inside = scene.plan({0.0f, 0.0f, 0.0f});
    REQUIRE(inside.size() == 1);
    REQUIRE(value_of(inside[0], "bloom_intensity") == Approx(1.0f));

    // Walking out through the falloff, the value moves a little at a time and
    // never jumps.
    float previous = value_of(inside[0], "bloom_intensity");
    for (float x = 7.0f; x <= 11.0f; x += 0.05f) {
        const auto layers = scene.plan({x, 0.0f, 0.0f});
        REQUIRE(layers.size() == 1);
        const float now = value_of(layers[0], "bloom_intensity");
        REQUIRE(now <= previous + 1e-6f);
        REQUIRE(previous - now < 0.05f);
        previous = now;
    }
    // Halfway through the falloff, halfway between the two.
    const auto half = scene.plan({9.0f, 0.0f, 0.0f});
    REQUIRE(value_of(half[0], "bloom_intensity") == Approx(0.65f));
}

TEST_CASE("A volume of another shader lays its image over the one below by its weight", "[post]") {
    Volumes scene;
    auto& v = scene.box(glm::vec3(0.0f), 10.0f, look_of(GRADE_SHADER, 5));
    v.edge_falloff = 0.2f;
    v.uniforms = {scalar("amount", 1.0f)};

    const auto half = scene.plan({9.0f, 0.0f, 0.0f});
    REQUIRE(half.size() == 2);
    REQUIRE(half[0].look.pipeline_id == DEFAULT_SHADER);
    REQUIRE(half[1].look.pipeline_id == GRADE_SHADER);
    REQUIRE(half[1].weight == Approx(0.5f));

    // Fully inside, the grade covers the default, so the default is not drawn.
    const auto inside = scene.plan({0.0f, 0.0f, 0.0f});
    REQUIRE(inside.size() == 1);
    REQUIRE(inside[0].look.pipeline_id == GRADE_SHADER);
    REQUIRE(inside[0].weight == 1.0f);
}

TEST_CASE("Different textures on one shader are different looks", "[post]") {
    Volumes scene;
    PostLook dusk = look_of(DEFAULT_SHADER, 6);
    dusk.textures[0] = 42;
    scene.box(glm::vec3(0.0f), 10.0f, dusk).weight = 0.4f;

    const auto layers = scene.plan(glm::vec3(0.0f));
    REQUIRE(layers.size() == 2);
    REQUIRE(layers[1].look.textures[0] == 42);
    REQUIRE(layers[1].weight == Approx(0.4f));
}

TEST_CASE("Higher priority applies over lower, whatever the scene order", "[post]") {
    Volumes scene;
    auto& high = scene.box(glm::vec3(0.0f), 10.0f, look_of(DEFAULT_SHADER), 5);
    high.uniforms = {scalar("bloom_intensity", 0.9f)};
    auto& low = scene.box(glm::vec3(0.0f), 10.0f, look_of(DEFAULT_SHADER), 1);
    low.uniforms = {scalar("bloom_intensity", 0.1f)};

    const auto layers = scene.plan(glm::vec3(0.0f));
    REQUIRE(layers.size() == 1);
    REQUIRE(value_of(layers[0], "bloom_intensity") == Approx(0.9f));
}

TEST_CASE("An infinite volume applies wherever the camera is, by its weight", "[post]") {
    Volumes scene;
    auto& base = scene.infinite(look_of(DEFAULT_SHADER));
    base.uniforms = {scalar("bloom_intensity", 0.5f)};
    base.weight = 0.5f;

    for (glm::vec3 camera : {glm::vec3(0.0f), glm::vec3(5000.0f, -300.0f, 12.0f)}) {
        const auto layers = scene.plan(camera);
        REQUIRE(layers.size() == 1);
        REQUIRE(value_of(layers[0], "bloom_intensity") == Approx(0.4f));
    }

    // Weight zero is a volume switched off.
    base.weight = 0.0f;
    REQUIRE(value_of(scene.plan(glm::vec3(0.0f))[0], "bloom_intensity") == Approx(0.3f));
}

TEST_CASE("A volume blends into the layer below it, not into the default under that", "[post]") {
    Volumes scene;
    auto& grade = scene.infinite(look_of(GRADE_SHADER), 0);
    grade.uniforms = {scalar("amount", 0.2f)};
    auto& room = scene.box(glm::vec3(0.0f), 10.0f, look_of(GRADE_SHADER), 1);
    room.uniforms = {scalar("amount", 1.0f)};
    room.edge_falloff = 0.2f;

    // The infinite grade covers the default; the room's values blend into
    // the grade's layer as the camera walks in.
    const auto half = scene.plan({9.0f, 0.0f, 0.0f});
    REQUIRE(half.size() == 1);
    REQUIRE(half[0].look.pipeline_id == GRADE_SHADER);
    REQUIRE(value_of(half[0], "amount") == Approx(0.6f));
}
