// input_api.hpp — Static input action API for game scripts
#pragma once

#include "fjell/api.h"
#include "fjell/input_action_ref.hpp"
#include "fjell/ref.hpp"

#include <glm/glm.hpp>

extern "C" {
FJELL_SDK_EXPORT void fjell_input_bind_void(uint32_t action_handle_id, int trigger,
                            void* instance, void (*fn)(void*));
FJELL_SDK_EXPORT void fjell_input_bind_float(uint32_t action_handle_id, int trigger,
                             void* instance, void (*fn)(void*, float));
FJELL_SDK_EXPORT void fjell_input_bind_vec2(uint32_t action_handle_id, int trigger,
                            void* instance, void (*fn)(void*, float, float));
FJELL_SDK_EXPORT void fjell_input_clear_connections();
FJELL_SDK_EXPORT void fjell_input_push_context(const char* name);
FJELL_SDK_EXPORT void fjell_input_pop_context(const char* name);
}

namespace fjell {

struct InputAPI {
    /// Bind a void() method.
    /// Usage: InputAPI::bind<&MyClass::on_fire>(action, EInputTrigger::Pressed, this);
    template <auto Method, typename T>
        requires requires(T* t) { (t->*Method)(); }
    static void bind(const Ref<InputAction>& action, EInputTrigger trigger, T* instance) {
        if (action.data_.handle_id == 0) return;
        fjell_input_bind_void(
            action.data_.handle_id, static_cast<int>(trigger), instance,
            [](void* self) { (static_cast<T*>(self)->*Method)(); });
    }

    /// Bind a void(float) method.
    template <auto Method, typename T>
        requires requires(T* t) { (t->*Method)(0.0f); }
    static void bind(const Ref<InputAction>& action, EInputTrigger trigger, T* instance) {
        if (action.data_.handle_id == 0) return;
        fjell_input_bind_float(
            action.data_.handle_id, static_cast<int>(trigger), instance,
            [](void* self, float v) { (static_cast<T*>(self)->*Method)(v); });
    }

    /// Bind a void(glm::vec2) method.
    template <auto Method, typename T>
        requires requires(T* t) { (t->*Method)(glm::vec2{}); }
    static void bind(const Ref<InputAction>& action, EInputTrigger trigger, T* instance) {
        if (action.data_.handle_id == 0) return;
        fjell_input_bind_vec2(
            action.data_.handle_id, static_cast<int>(trigger), instance,
            [](void* self, float x, float y) {
                (static_cast<T*>(self)->*Method)(glm::vec2{x, y});
            });
    }

    static void push_context(const char* name) { fjell_input_push_context(name); }
    static void pop_context(const char* name) { fjell_input_pop_context(name); }
};

} // namespace fjell
