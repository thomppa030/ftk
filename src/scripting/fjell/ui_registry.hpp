// fjell_ui_registry.hpp — Type registration for UiWidget game .so files
#pragma once

#include "fjell/api.h"
#include "fjell/ui_widget.hpp"
#include "fjell/registry.hpp"

#include <cstdint>

// ── UI widget type descriptor (C-compatible) ──────────────────────────

struct FjellUiWidgetTypeOOP {
    const char* name;
    fjell::UiWidget* (*create)();
    const FjellFieldDesc* fields;
    uint32_t field_count;
    const FjellUiActionDesc* actions;
    uint32_t action_count;
};

// Backward compat alias
using FjellUiComponentTypeOOP = FjellUiWidgetTypeOOP;

// ── Generated reflection info (filled by fjreflect codegen) ──────────────

struct FjellUiGeneratedInfo {
    const char* name;
    const FjellFieldDesc* fields;
    uint32_t field_count;
    const FjellUiActionDesc* actions;
    uint32_t action_count;
};

// ── Engine exports ───────────────────────────────────────────────────────

struct FjellTypeRegistry;
extern "C" FJELL_SDK_EXPORT void fjell_reg_ui_component(FjellTypeRegistry* reg, const FjellUiWidgetTypeOOP* desc);
extern "C" FJELL_SDK_EXPORT void fjell_set_ui_generated_table(const FjellUiGeneratedInfo* table, uint32_t count);
extern "C" FJELL_SDK_EXPORT const FjellUiGeneratedInfo* fjell_find_ui_generated(const char* name);

namespace fjell {

// ── Registration helper ──────────────────────────────────────────────────

template <typename T>
void register_ui_widget(FjellTypeRegistry* reg) {
    FjellUiWidgetTypeOOP desc{};
    desc.name = T::type_name;
    desc.create = []() -> UiWidget* { return new T(); };

    const auto* gen = fjell_find_ui_generated(T::type_name);
    if (gen) {
        desc.fields = gen->fields;
        desc.field_count = gen->field_count;
        desc.actions = gen->actions;
        desc.action_count = gen->action_count;
    }

    fjell_reg_ui_component(reg, &desc);
}

// Backward compat
template <typename T>
void register_ui_component(FjellTypeRegistry* reg) { register_ui_widget<T>(reg); }

} // namespace fjell

// ── Auto-registration (FjellUiAutoReg + head declared in registry.hpp) ──

// ── Macros ────────────────────────────────────────────────────────────────

/// Declares a class as a UI widget. Place it on the first line of the class
/// body of a `UiWidget` subclass, with the class's own name as the argument.
///
/// Registers the type so `.fjwidget` assets can reference it by name. The
/// macro leaves the class in a `public:` section.
///
/// @param T The class name, spelled exactly as declared.
/// @code
/// class PauseMenu : public fjell::UiWidget {
///     FJELL_UI_WIDGET(PauseMenu)
/// public:
///     [[fj::action]] void on_resume() { fjell::Time::resume(); }
/// };
/// @endcode
#define FJELL_UI_WIDGET(T) \
    public: static constexpr const char* type_name = #T; \
    template <class _FjAccessT> friend struct ::fjell::FjellAccess; \
    private: \
    static inline FjellUiAutoReg _fjell_ui_reg_{ \
        [](FjellTypeRegistry* r) { fjell::register_ui_widget<T>(r); }, \
        nullptr}; \
    struct _FjellUiRegInit { _FjellUiRegInit() { \
        _fjell_ui_reg_.next = fjell_ui_auto_reg_head(); \
        fjell_ui_auto_reg_head() = &_fjell_ui_reg_; \
    }}; \
    static inline _FjellUiRegInit _fjell_ui_reg_init_{}; \
    public:

/// @deprecated Use `FJELL_UI_WIDGET(T)`. Kept for older projects; the extra
///             tag argument is accepted and ignored.
#define FJELL_UI_COMPONENT(T, ...) FJELL_UI_WIDGET(T)
