#include "scene/component_registry.hpp"

namespace fjell {

ComponentRegistry& ComponentRegistry::instance() {
    static ComponentRegistry registry;
    return registry;
}

} // namespace fjell
