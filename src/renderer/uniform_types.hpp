#pragma once

#include <glm/glm.hpp>

namespace fjell {

struct GlobalUbo {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 projection;
};

} // namespace fjell
