// animator_api.hpp — Skeletal animation API for game scripts
#pragma once

#include "fjell/api.h"

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT void  fjell_animator_set_param(uint32_t node_id, const char* name, float value);
FJELL_SDK_EXPORT float fjell_animator_get_param(uint32_t node_id, const char* name);
FJELL_SDK_EXPORT const char* fjell_animator_current_state(uint32_t node_id);
}

namespace fjell {

struct Animator {
    static void set_param(uint32_t node_id, const char* name, float value) {
        fjell_animator_set_param(node_id, name, value);
    }

    static float get_param(uint32_t node_id, const char* name) {
        return fjell_animator_get_param(node_id, name);
    }

    static const char* current_state(uint32_t node_id) {
        return fjell_animator_current_state(node_id);
    }
};

} // namespace fjell
