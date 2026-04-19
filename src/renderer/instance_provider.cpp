#include "renderer/instance_provider.hpp"

namespace fjell {

InstanceProviderRegistry& InstanceProviderRegistry::instance() {
    static InstanceProviderRegistry r;
    return r;
}

} // namespace fjell
