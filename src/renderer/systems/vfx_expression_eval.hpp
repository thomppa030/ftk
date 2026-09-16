#pragma once

#include <glm/glm.hpp>

namespace fjell {

struct VfxEmitter;

/// Evaluate the expression graph for one emitter, writing results
/// into module member variables via PinDescriptors.
/// Call before write_gpu_modules(). Skips if no links exist.
/// `wind` is the environment's wind, direction times speed with the gust,
/// for the Global Wind source node.
void evaluate_expression_graph(VfxEmitter& emitter, float time, float dt, const glm::vec3& wind);

} // namespace fjell
