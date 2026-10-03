#include "ftk/ui/kit/state.hpp"

#include "ftk/ui/kit/core/kit.hpp"

namespace ftk::ui {

using namespace corekit;

// Under ids made in the window being drawn, so one window's are its own.

int stored_int(Id id, int fallback) {
    return ui().storage().get_int(id, fallback);
}

bool stored_bool(Id id, bool fallback) {
    return ui().storage().get_bool(id, fallback);
}

float stored_float(Id id, float fallback) {
    return ui().storage().get_float(id, fallback);
}

void store_int(Id id, int value) {
    ui().storage().set_int(id, value);
}

void store_bool(Id id, bool value) {
    ui().storage().set_bool(id, value);
}

void store_float(Id id, float value) {
    ui().storage().set_float(id, value);
}

} // namespace ftk::ui
