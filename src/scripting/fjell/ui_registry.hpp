// fjell_ui_registry.hpp — Type registration for UiComponent game .so files
#pragma once

#include "fjell/ui_component.hpp"
#include "fjell/registry.hpp"

#include <cstdint>

// ── UI component type descriptor (C-compatible) ──────────────────────────

struct FjellUiComponentTypeOOP {
    const char* name;
    const char* widget_tag;  // e.g., "health-bar"
    fjell::UiComponent* (*create)();
    const FjellFieldDesc* fields;
    uint32_t field_count;
    void (*trace)(void* instance, void* visitor);
};

// ── Generated reflection info (filled by fjreflect codegen) ──────────────

struct FjellUiGeneratedInfo {
    const char* name;
    const char* widget_tag;
    const FjellFieldDesc* fields;
    uint32_t field_count;
    void (*trace)(void* instance, void* visitor);
};

// ── Engine exports ───────────────────────────────────────────────────────

struct FjellTypeRegistry;
extern "C" void fjell_reg_ui_component(FjellTypeRegistry* reg, const FjellUiComponentTypeOOP* desc);
extern "C" const FjellUiGeneratedInfo* fjell_find_ui_generated(const char* name);

namespace fjell {

// ── Registration helper ──────────────────────────────────────────────────

template <typename T>
void register_ui_component(FjellTypeRegistry* reg) {
    FjellUiComponentTypeOOP desc{};
    desc.name = T::type_name;
    desc.widget_tag = T::widget_tag_name;
    desc.create = []() -> UiComponent* { return new T(); };

    const auto* gen = fjell_find_ui_generated(T::type_name);
    if (gen) {
        desc.fields = gen->fields;
        desc.field_count = gen->field_count;
        desc.trace = gen->trace;
    }

    fjell_reg_ui_component(reg, &desc);
}

} // namespace fjell

// ── Auto-registration (FjellUiAutoReg + head declared in registry.hpp) ──

// ── Macro ────────────────────────────────────────────────────────────────

#define FJELL_UI_COMPONENT(T, tag) \
    public: static constexpr const char* type_name = #T; \
    public: static constexpr const char* widget_tag_name = tag; \
    private: \
    static inline FjellUiAutoReg _fjell_ui_reg_{ \
        [](FjellTypeRegistry* r) { fjell::register_ui_component<T>(r); }, \
        nullptr}; \
    struct _FjellUiRegInit { _FjellUiRegInit() { \
        _fjell_ui_reg_.next = fjell_ui_auto_reg_head(); \
        fjell_ui_auto_reg_head() = &_fjell_ui_reg_; \
    }}; \
    static inline _FjellUiRegInit _fjell_ui_reg_init_{}; \
    public:
