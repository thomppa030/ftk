#pragma once

#include "ui/kit/edit.hpp"

namespace fjell {

struct InspectorContext;
struct VfxModule;

/// A VFX module's fields as property rows, its asset references among them,
/// for the Module Stack. The module holds only its data; how it is edited
/// lives here.
ui::Edit draw_vfx_module_fields(VfxModule& module, const InspectorContext& ctx);

} // namespace fjell
