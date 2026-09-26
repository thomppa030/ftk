#pragma once

#include <cstdint>
#include <memory>
#include <utility>

namespace fjell {

using ComponentTypeId = uint32_t;

// Global counter for compile-time type IDs.
// Each unique type gets a unique ID on first call to component_type_id<T>().
inline ComponentTypeId next_component_id() {
    static ComponentTypeId counter{0};
    return counter++;
}

template <typename T>
ComponentTypeId component_type_id() {
    static const ComponentTypeId id = next_component_id();
    return id;
}

class ComponentStorage;

// How a component is duplicated when its node is. The default copy-constructs,
// which is right for plain data. A type whose copy would be wrong — runtime
// handles that must not be shared, state that has to be rebuilt against a
// sibling component, or no copy constructor at all — declares an overload
// next to its own definition. `from` is the storage of the node being copied,
// for rules that need a sibling. A type without a copy constructor and without
// an overload fails to compile, so no component can be forgotten.
template <typename T>
void clone_component(const ComponentStorage& from, const T& src, ComponentStorage& to);

/// What code that holds components without knowing their type needs to know
/// about one: its id, the memory it takes, how to end it, move it and copy it
/// by its clone rule. There is one per component type, made the first time a
/// component of the type is created. Looking a component up compares ids and
/// never needs this, so code that only reads a type does not pull in how to
/// destroy or copy it.
struct ComponentType {
    ComponentTypeId id;
    uint32_t size;
    uint32_t align;
    /// Runs the component's destructor. The memory stays with its owner.
    void (*destroy)(void* component);
    /// Move-constructs the component at `from` into the uninitialised memory
    /// at `to`, then ends the one at `from`. A component moves only when its
    /// node joins or leaves a scene. Null for a script type: a script never
    /// moves, and is made again from its saved fields instead.
    void (*relocate)(void* to, void* from);
    /// Null for a script type, which is copied through its saved fields.
    void (*clone)(const ComponentStorage& from, const void* src, ComponentStorage& to);
};

template <typename T>
const ComponentType& component_type() {
    // Components live in plain byte allocations, which are aligned this far
    // and no further.
    static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
                  "a component may not need more alignment than operator new gives");
    static const ComponentType type{
        component_type_id<T>(),
        static_cast<uint32_t>(sizeof(T)),
        static_cast<uint32_t>(alignof(T)),
        [](void* component) { std::destroy_at(static_cast<T*>(component)); },
        [](void* to, void* from) {
            T* source = static_cast<T*>(from);
            std::construct_at(static_cast<T*>(to), std::move(*source));
            std::destroy_at(source);
        },
        [](const ComponentStorage& from, const void* src, ComponentStorage& to) {
            clone_component(from, *static_cast<const T*>(src), to);
        },
    };
    return type;
}

} // namespace fjell
