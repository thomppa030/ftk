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
/// It also pins a shader's own rules where there is no CPU half, as
/// test_post_effects.cpp and test_rt_hit.cpp do.
///
/// Only for shaders that are functions over vectors and scalars, with no
/// stage builtins. Whatever such an include reads from a buffer or sampler
/// it reads through names its caller provides, which a test defines before
/// including it.

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

using vec2 = glm::vec2;
using vec3 = glm::vec3;
using vec4 = glm::vec4;
using mat3 = glm::mat3;
using mat4 = glm::mat4;
using mat4x3 = glm::mat4x3;
using ivec2 = glm::ivec2;
using ivec3 = glm::ivec3;
using ivec4 = glm::ivec4;
using uvec2 = glm::uvec2;
using uvec3 = glm::uvec3;
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
using glm::transpose;

// The float overloads of <cmath>, brought in rather than defined here: MSVC
// declares them in the global namespace itself.
using std::abs;
using std::cos;
using std::exp;
using std::floor;
using std::pow;
using std::sin;
using std::sqrt;

/// Only marks an index as varying across invocations on the GPU.
template <typename T> T nonuniformEXT(T index) { return index; }

inline float max(float a, float b) { return std::max(a, b); }
inline float min(float a, float b) { return std::min(a, b); }
inline unsigned int min(unsigned int a, unsigned int b) { return std::min(a, b); }
inline float clamp(float v, float lo, float hi) { return std::clamp(v, lo, hi); }
inline vec3 sqrt(vec3 v) { return glm::sqrt(v); }
inline float fract(float v) { return v - std::floor(v); }
inline float mod(float x, float y) { return x - y * std::floor(x / y); }
inline vec3 pow(vec3 a, vec3 b) { return glm::pow(a, b); }
inline float mix(float a, float b, float t) { return a + (b - a) * t; }
inline float inversesqrt(float v) { return 1.0f / std::sqrt(v); }
inline float smoothstep(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
