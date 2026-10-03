#pragma once

// The harness the kit's tests drive the kit with: the kit on the UI core,
// in ftk-kit-harness, which programs' tests use as well. The tests built
// against the kit's ImGui implementation have gone (Phase 9 of the UI core
// plan); a test's FTK_KIT_ON_CORE branches are the ones that run.

#include "ftk/test/kit_harness.hpp"
