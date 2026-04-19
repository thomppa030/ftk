#include "core/math/spline.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

using namespace fjell;
using Catch::Approx;

TEST_CASE("Empty spline evaluates to zero", "[spline]") {
    Spline<float> s;
    REQUIRE(s.evaluate(0.0F) == 0.0F);
    REQUIRE(s.evaluate(1.0F) == 0.0F);
    REQUIRE(s.total_length() == 0.0F);
}

TEST_CASE("Single-point spline holds its value", "[spline]") {
    Spline<float> s;
    s.add_point(0.0F, 7.0F);
    REQUIRE(s.evaluate(-5.0F) == 7.0F);
    REQUIRE(s.evaluate( 0.0F) == 7.0F);
    REQUIRE(s.evaluate( 5.0F) == 7.0F);
}

TEST_CASE("Linear mode interpolates linearly", "[spline]") {
    Spline<float> s;
    s.add_point(0.0F, 0.0F, SplineInterp::Linear);
    s.add_point(1.0F, 10.0F, SplineInterp::Linear);
    REQUIRE(s.evaluate(0.0F)  == Approx(0.0F));
    REQUIRE(s.evaluate(0.25F) == Approx(2.5F));
    REQUIRE(s.evaluate(0.5F)  == Approx(5.0F));
    REQUIRE(s.evaluate(1.0F)  == Approx(10.0F));
}

TEST_CASE("Constant mode holds the left value", "[spline]") {
    Spline<float> s;
    s.add_point(0.0F, 3.0F, SplineInterp::Constant);
    s.add_point(1.0F, 9.0F, SplineInterp::Constant);
    REQUIRE(s.evaluate(0.0F)  == 3.0F);
    REQUIRE(s.evaluate(0.5F)  == 3.0F);
    REQUIRE(s.evaluate(0.99F) == 3.0F);
    REQUIRE(s.evaluate(1.0F)  == 9.0F);
}

TEST_CASE("CurveAuto passes through control points", "[spline]") {
    Spline<float> s;
    s.add_point(0.0F, 0.0F);
    s.add_point(1.0F, 2.0F);
    s.add_point(2.0F, 1.0F);
    s.add_point(3.0F, 3.0F);
    s.auto_set_tangents();

    REQUIRE(s.evaluate(0.0F) == Approx(0.0F));
    REQUIRE(s.evaluate(1.0F) == Approx(2.0F));
    REQUIRE(s.evaluate(2.0F) == Approx(1.0F));
    REQUIRE(s.evaluate(3.0F) == Approx(3.0F));
}

TEST_CASE("Evaluation clamps outside the key range", "[spline]") {
    Spline<float> s;
    s.add_point(1.0F, 5.0F, SplineInterp::Linear);
    s.add_point(2.0F, 8.0F, SplineInterp::Linear);
    REQUIRE(s.evaluate(-10.0F) == Approx(5.0F));
    REQUIRE(s.evaluate( 99.0F) == Approx(8.0F));
}

TEST_CASE("add_point keeps input_key monotonic", "[spline]") {
    Spline<float> s;
    s.add_point(2.0F, 20.0F);
    s.add_point(0.0F, 0.0F);
    s.add_point(1.0F, 10.0F);
    REQUIRE(s.points.size() == 3);
    REQUIRE(s.points[0].input_key == 0.0F);
    REQUIRE(s.points[1].input_key == 1.0F);
    REQUIRE(s.points[2].input_key == 2.0F);
}

TEST_CASE("CurveUser honors authored tangents", "[spline]") {
    Spline<float> s;
    s.add_point(0.0F, 0.0F, SplineInterp::CurveUser);
    s.add_point(1.0F, 0.0F, SplineInterp::CurveUser);
    // Equal values, opposite tangents: curve bulges symmetrically.
    // Hermite midpoint with p0=p1=0, m0=+k, m1=-k is k * (h10 - h11) = k * 0.25.
    s.points[0].leave_tangent  =  4.0F;
    s.points[1].arrive_tangent = -4.0F;
    REQUIRE(s.evaluate(0.5F) == Approx(1.0F));

    // Flat tangents on equal-valued endpoints = flat line.
    s.points[0].leave_tangent  = 0.0F;
    s.points[1].arrive_tangent = 0.0F;
    REQUIRE(s.evaluate(0.5F) == Approx(0.0F));
}

TEST_CASE("tangent() matches Linear slope", "[spline]") {
    Spline<float> s;
    s.add_point(0.0F, 0.0F, SplineInterp::Linear);
    s.add_point(2.0F, 10.0F, SplineInterp::Linear);
    REQUIRE(s.tangent(0.5F) == Approx(5.0F));
    REQUIRE(s.tangent(1.5F) == Approx(5.0F));
}

TEST_CASE("vec3 spline interpolates componentwise", "[spline]") {
    Spline<glm::vec3> s;
    s.add_point(0.0F, glm::vec3(0.0F), SplineInterp::Linear);
    s.add_point(1.0F, glm::vec3(1.0F, 2.0F, 3.0F), SplineInterp::Linear);
    const auto v = s.evaluate(0.5F);
    REQUIRE(v.x == Approx(0.5F));
    REQUIRE(v.y == Approx(1.0F));
    REQUIRE(v.z == Approx(1.5F));
}

TEST_CASE("Reparam table gives total length of a straight line", "[spline]") {
    Spline<glm::vec3> s;
    s.add_point(0.0F, glm::vec3(0.0F),             SplineInterp::Linear);
    s.add_point(1.0F, glm::vec3(3.0F, 4.0F, 0.0F), SplineInterp::Linear);
    s.build_reparam_table(64);
    REQUIRE(s.total_length() == Approx(5.0F).epsilon(0.001F));
}

TEST_CASE("evaluate_at_distance matches arc position on a line", "[spline]") {
    Spline<glm::vec3> s;
    s.add_point(0.0F, glm::vec3(0.0F),             SplineInterp::Linear);
    s.add_point(1.0F, glm::vec3(3.0F, 4.0F, 0.0F), SplineInterp::Linear);
    s.build_reparam_table(256);

    // halfway along length 5 should land at (1.5, 2.0, 0.0)
    const auto mid = s.evaluate_at_distance(2.5F);
    REQUIRE(mid.x == Approx(1.5F).epsilon(0.01F));
    REQUIRE(mid.y == Approx(2.0F).epsilon(0.01F));
}

TEST_CASE("remove_point shrinks the spline", "[spline]") {
    Spline<float> s;
    s.add_point(0.0F, 0.0F);
    s.add_point(1.0F, 1.0F);
    s.add_point(2.0F, 2.0F);
    s.remove_point(1);
    REQUIRE(s.points.size() == 2);
    REQUIRE(s.points[0].value == 0.0F);
    REQUIRE(s.points[1].value == 2.0F);
}

TEST_CASE("closed spline extends max_key by a closing segment span", "[spline]") {
    Spline<float> s;
    s.add_point(0.0F, 0.0F);
    s.add_point(1.0F, 1.0F);
    s.add_point(2.0F, 2.0F);
    REQUIRE(s.max_key() == Approx(2.0F));
    s.closed = true;
    // Last authored segment is [1, 2] → closing span = 1.0
    REQUIRE(s.max_key() == Approx(3.0F));
    REQUIRE(s.closing_span() == Approx(1.0F));
}

TEST_CASE("closed spline evaluates closing segment back to first point", "[spline]") {
    Spline<glm::vec3> s;
    s.add_point(0.0F, glm::vec3(0.0F, 0.0F, 0.0F), SplineInterp::Linear);
    s.add_point(1.0F, glm::vec3(1.0F, 0.0F, 0.0F), SplineInterp::Linear);
    s.add_point(2.0F, glm::vec3(1.0F, 1.0F, 0.0F), SplineInterp::Linear);
    s.closed = true;
    // At max_key we should be back at points.front()
    const auto end = s.evaluate(s.max_key());
    REQUIRE(end.x == Approx(0.0F));
    REQUIRE(end.y == Approx(0.0F));
    REQUIRE(end.z == Approx(0.0F));
    // Halfway through the closing segment (2.5 of span 1), we're halfway
    // between (1, 1, 0) and (0, 0, 0)
    const auto mid = s.evaluate(2.5F);
    REQUIRE(mid.x == Approx(0.5F));
    REQUIRE(mid.y == Approx(0.5F));
}

TEST_CASE("closed triangle reparam length equals perimeter", "[spline]") {
    Spline<glm::vec3> s;
    s.add_point(0.0F, glm::vec3(0.0F, 0.0F, 0.0F), SplineInterp::Linear);
    s.add_point(1.0F, glm::vec3(3.0F, 0.0F, 0.0F), SplineInterp::Linear);
    s.add_point(2.0F, glm::vec3(3.0F, 4.0F, 0.0F), SplineInterp::Linear);
    s.closed = true;
    s.build_reparam_table(256);
    // Perimeter: 3 + 4 + 5 = 12
    REQUIRE(s.total_length() == Approx(12.0F).epsilon(0.01F));
}
