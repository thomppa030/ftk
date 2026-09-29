#pragma once

/// Lets a test `#include` a GLSL source file and call its functions as C++.
///
/// GLSL and C++ share enough syntax that a shader written in plain functions
/// over vectors compiles as C++ once its vocabulary is in scope: GLM already
/// provides the vector types and their overloads, and what is missing is the
/// scalar half of the builtins (`max(float, float)` and friends), which GLM
/// leaves to <algorithm> and <cmath>.
///
/// This exists so a shader that has a CPU counterpart can be tested against
/// it for real. Compiling both halves and comparing their output at sample
/// points is the only check that actually catches the formulas drifting
/// apart; compiling the shader alone proves it parses, not that it still
/// computes what the CPU computes.
///
/// Only for shaders that are pure functions over vectors and scalars — no
/// bindings, no samplers, no stage builtins.

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

using vec2 = glm::vec2;
using vec3 = glm::vec3;
using vec4 = glm::vec4;
using mat3 = glm::mat3;
using mat4 = glm::mat4;
using ivec2 = glm::ivec2;
using ivec3 = glm::ivec3;
using uvec2 = glm::uvec2;
using uvec4 = glm::uvec4;
using uint = unsigned int;

using glm::abs;
using glm::clamp;
using glm::cross;
using glm::dot;
using glm::length;
using glm::max;
using glm::min;
using glm::mix;
using glm::normalize;

inline float max(float a, float b) { return std::max(a, b); }
inline float min(float a, float b) { return std::min(a, b); }
inline unsigned int min(unsigned int a, unsigned int b) { return std::min(a, b); }
inline float clamp(float v, float lo, float hi) { return std::clamp(v, lo, hi); }
inline float sqrt(float v) { return std::sqrt(v); }
inline vec3 sqrt(vec3 v) { return glm::sqrt(v); }
inline float cos(float v) { return std::cos(v); }
inline float sin(float v) { return std::sin(v); }
inline float abs(float v) { return std::abs(v); }
inline float floor(float v) { return std::floor(v); }
inline float fract(float v) { return v - std::floor(v); }
inline float mod(float x, float y) { return x - y * std::floor(x / y); }
inline float pow(float a, float b) { return std::pow(a, b); }
inline vec3 pow(vec3 a, vec3 b) { return glm::pow(a, b); }
inline float mix(float a, float b, float t) { return a + (b - a) * t; }
inline float exp(float v) { return std::exp(v); }
inline float inversesqrt(float v) { return 1.0f / std::sqrt(v); }
inline float smoothstep(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
