#pragma once

#include "scene/node_component.hpp"

namespace fjell {

/// Marks a node as something the world notices moving: it writes into the
/// motion field, which anything reacting to movement reads — fog stirred by a
/// runner, grass bent aside, a track through snow.
///
/// A character gets one by default when physics gives it a capsule, but
/// nothing about this is character-shaped. A vehicle, a rolling boulder or a
/// spell effect stirs the world by carrying this component, rather than the
/// gathering growing another case for each new kind of thing.
struct MotionSourceComponent : NodeComponent {
    /// How wide a mark it leaves, in metres. Left at zero it follows the
    /// node's own size where there is one to follow — a character's capsule —
    /// and falls back to half a metre where there is not.
    float radius{0.0f};
    /// How far above the node the movement reaches, so a consumer filling a
    /// volume can leave the air above a walker's head alone. Zero follows the
    /// node's size the same way.
    float height{0.0f};
    /// Scales the mark this leaves. Below one for something that barely
    /// disturbs the air, above it for something that shoulders through.
    float strength{1.0f};
};

} // namespace fjell
