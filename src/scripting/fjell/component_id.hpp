// component_id.hpp — The id the engine knows a component type by
#pragma once

#include "fjell/api.h"

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT uint32_t fjell_component_type_id(const char* type_name);
}

namespace fjell::detail {

/// The engine's id for component type T, an engine component or one of this
/// library's scripts: what the node and scene lookups pass in place of the
/// type's name. Asked once per type for the life of the library, which the
/// engine keeps the answer for. A type the engine does not know reads as
/// FJELL_NO_COMPONENT_TYPE, under which no lookup finds anything.
template <typename T>
[[nodiscard]] uint32_t component_type_id() {
    static const uint32_t id = fjell_component_type_id(T::type_name);
    return id;
}

} // namespace fjell::detail
