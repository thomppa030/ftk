#pragma once

#include "ftk/ui/kit/edit.hpp"
#include "ftk/ui/theme.hpp"
#include "scene/component_meta.hpp"
#include "scene/component_storage.hpp"

#include <vector>

// How the editor shows each kind of component. The runtime describes a
// component in its ComponentMeta (name, saving, scripting); what it looks
// like in the Properties panel, and which category it belongs to, is the
// editor's and lives here.

namespace fjell {

struct Node3D;

struct ComponentInspector {
    /// The component's domain: the colour of its bar, and its group when
    /// adding a component.
    ftk::theme::Category category{ftk::theme::Category::Structure};
    /// Draws the component's fields into its block.
    ftk::ui::Edit (*draw)(Node3D& node, void* component, const InspectorContext& ctx){nullptr};
};

class InspectorRegistry {
public:
    template <typename T>
    void add(ComponentInspector inspector) {
        entries_.push_back({component_type_id<T>(), inspector});
    }

    /// The inspector for a component type, or null when it has none.
    [[nodiscard]] const ComponentInspector* find(ComponentTypeId id) const;

private:
    struct Entry {
        ComponentTypeId id;
        ComponentInspector inspector;
    };
    std::vector<Entry> entries_;
};

/// Every component's inspector, listed in one place and built on first use.
[[nodiscard]] const InspectorRegistry& component_inspectors();

} // namespace fjell
