// animator_api.hpp — Skeletal animation API for game scripts
#pragma once

#include <cstdint>

extern "C" {
void  fjell_animator_set_param(uint32_t node_id, const char* name, float value);
float fjell_animator_get_param(uint32_t node_id, const char* name);
const char* fjell_animator_current_state(uint32_t node_id);
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
