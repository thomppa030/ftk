#pragma once

// Dear ImGui's configuration for everything built on ftk, named by
// IMGUI_USER_CONFIG on ftk's ImGui targets so that every file including
// imgui.h sees it.
//
// ImGui's vectors and glm's convert both ways: ImVec2 with glm::vec2 for
// points and sizes, ImVec4 with glm::vec4 for colours. The kit's headers
// move from ImGui's types to glm's, and code on either side keeps compiling
// while the rest of the editor moves over. The bridge goes with ImGui.

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#define IM_VEC2_CLASS_EXTRA                                                                        \
    constexpr ImVec2(const glm::vec2& v) : x(v.x), y(v.y) {}                                      \
    operator glm::vec2() const { return {x, y}; }

#define IM_VEC4_CLASS_EXTRA                                                                        \
    constexpr ImVec4(const glm::vec4& v) : x(v.x), y(v.y), z(v.z), w(v.w) {}                      \
    operator glm::vec4() const { return {x, y, z, w}; }
