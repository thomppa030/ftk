#include "scene/component_storage.hpp"

#include "ftk/base/log.hpp"
#include "scene/component_registry.hpp"
#include "scene/node.hpp"

namespace fjell {

void ComponentStorage::report_second(ComponentTypeId id) const {
    const ComponentMeta* meta = ComponentRegistry::instance().find(id);
    const std::string what = meta != nullptr ? meta->display_name : std::string("component of that type");
    if (owner_ != nullptr) {
        FJELL_CORE_WARN("'{}' already has a {}, so adding another gives back that one", owner_->name, what);
    } else {
        FJELL_CORE_WARN("A node already has a {}, so adding another gives back that one", what);
    }
}

} // namespace fjell
