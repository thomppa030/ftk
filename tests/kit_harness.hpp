#pragma once

// The harness the kit's tests drive the kit with. Both take the kit's own
// types and read the same, so a kit test is written once: KitHarness is
// ImGuiHarness while the tests run against the kit's ImGui implementation,
// and UiHarness when they are built against the core's (FTK_KIT_ON_CORE,
// Phase 6 of the UI core plan).

#ifdef FTK_KIT_ON_CORE
#include "ftk/test/ui_harness.hpp"
#else
#include "ftk/test/imgui_harness.hpp"
#endif

namespace ftk::test {

#ifdef FTK_KIT_ON_CORE
using KitHarness = UiHarness;
#else
using KitHarness = ImGuiHarness;
#endif

} // namespace ftk::test
