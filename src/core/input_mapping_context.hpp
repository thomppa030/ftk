#pragma once

#include "core/input_binding.hpp"

#include <string>
#include <vector>

namespace fjell {

struct InputMappingContext {
    std::string name;
    bool consume{true};
    std::vector<InputBinding> bindings;
};

} // namespace fjell
