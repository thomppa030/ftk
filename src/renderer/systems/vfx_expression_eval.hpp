#pragma once

namespace fjell {

struct VfxEmitter;

/// Evaluate the expression graph for one emitter, writing results
/// into module member variables via PinDescriptors.
/// Call before write_gpu_modules(). Skips if no links exist.
void evaluate_expression_graph(VfxEmitter& emitter, float time, float dt);

} // namespace fjell
