#include "scene/spline_editable.hpp"

namespace fjell {

SplineEditableRegistry& SplineEditableRegistry::instance() {
    static SplineEditableRegistry r;
    return r;
}

} // namespace fjell
