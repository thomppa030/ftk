#pragma once

#include <cstdint>

namespace fjell {

class Scene;

// Legacy VFX node graph — replaced by VfxEditorContext.
// Kept as a stub to avoid breaking SceneEditorContext references.
class NodeEditorPanel {
public:
    void init(Scene* /*scene*/, uint32_t* /*selected_id*/) {}
    void shutdown() {}
    void draw() {}

    void set_visible(bool v) { visible_ = v; }
    [[nodiscard]] bool visible() const { return visible_; }

private:
    bool visible_{false};
};

} // namespace fjell
