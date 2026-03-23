#pragma once

#include "scene/scene.hpp"

#include <memory>
#include <vector>

namespace fjell {

class Input;

class PlaySession {
public:
    explicit PlaySession(Scene& scene);

    [[nodiscard]] bool wants_stop(const Input& input) const;

    void restore(Scene& scene);

private:
    std::vector<std::unique_ptr<Node>> scene_snapshot_;
    uint32_t next_id_snapshot_;
};

} // namespace fjell
