// fjell_registry.hpp — Type registration for Fjell game .so files
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include "fjell/component.hpp"
#include "fjell/notify.hpp"
#include "fjell/api.h"

#include <cstdint>

// ── Type descriptors (C-compatible structs) ─────────────────────────────

struct FjellComponentTypeOOP {
    const char* name;
    fjell::ScriptComponent* (*create)();
    const FjellFieldDesc* fields;
    uint32_t field_count;
    void (*trace)(void* instance, void* visitor);  // GC trace (null if no ref fields)
};

struct FjellNotifyTypeOOP {
    const char* name;
    fjell::AnimNotify* (*create)();
    const FjellFieldDesc* fields;
    uint32_t field_count;
    void (*trace)(void* instance, void* visitor);
    bool is_state;  // true for AnimNotifyState subclasses
};

// ── Generated reflection info (filled by fjreflect codegen) ─────────────

struct FjellGeneratedInfo {
    const char* name;
    const FjellFieldDesc* fields;
    uint32_t field_count;
    void (*trace)(void* instance, void* visitor);
    const FjellRpcDesc* rpcs;   // null if the component has no [[fj::rpc]] methods
    uint32_t rpc_count;
};

// ── Opaque registry handle (engine-side) ────────────────────────────────

struct FjellTypeRegistry;

// Engine exports these via -rdynamic — game .so calls them at registration
extern "C" FJELL_SDK_EXPORT void fjell_reg_component(FjellTypeRegistry* reg, const FjellComponentTypeOOP* desc);
extern "C" FJELL_SDK_EXPORT void fjell_reg_notify(FjellTypeRegistry* reg, const FjellNotifyTypeOOP* desc);
extern "C" FJELL_SDK_EXPORT void fjell_set_generated_table(const FjellGeneratedInfo* table, uint32_t count);
extern "C" FJELL_SDK_EXPORT const FjellGeneratedInfo* fjell_find_generated(const char* name);

namespace fjell {

// ── Registration helper (called from game code) ─────────────────────────

template <typename T>
void register_component(FjellTypeRegistry* reg) {
    FjellComponentTypeOOP desc{};
    desc.name = T::type_name;
    desc.create = []() -> ScriptComponent* { return new T(); };

    // Fields and trace come from fjreflect codegen
    const auto* gen = fjell_find_generated(T::type_name);
    if (gen) {
        desc.fields = gen->fields;
        desc.field_count = gen->field_count;
        desc.trace = gen->trace;
    }

    fjell_reg_component(reg, &desc);
}

template <typename T>
void register_notify(FjellTypeRegistry* reg) {
    FjellNotifyTypeOOP desc{};
    desc.name = T::type_name;
    desc.create = []() -> AnimNotify* { return new T(); };
    desc.is_state = std::is_base_of_v<AnimNotifyState, T>;

    const auto* gen = fjell_find_generated(T::type_name);
    if (gen) {
        desc.fields = gen->fields;
        desc.field_count = gen->field_count;
        desc.trace = gen->trace;
    }

    fjell_reg_notify(reg, &desc);
}

} // namespace fjell

// ── Auto-registration ───────────────────────────────────────────────────
// Each FJELL_COMPONENT / FJELL_UI_COMPONENT builds a linked list of
// registration functions at static-init time. fjell_register() walks both
// lists when the engine loads the .so. No central register file needed.

struct FjellAutoReg {
    using Fn = void(*)(FjellTypeRegistry*);
    Fn fn;
    FjellAutoReg* next;
};

inline FjellAutoReg*& fjell_auto_reg_head() {
    static FjellAutoReg* head = nullptr;
    return head;
}

// UI component auto-registration (full definition here so both
// FJELL_REGISTER and FJELL_UI_COMPONENT can use it as a complete type)
struct FjellUiAutoReg {
    using Fn = void(*)(FjellTypeRegistry*);
    Fn fn;
    FjellUiAutoReg* next;
};

inline FjellUiAutoReg*& fjell_ui_auto_reg_head() {
    static FjellUiAutoReg* head = nullptr;
    return head;
}

// Notify auto-registration (parallel to component and UI)
struct FjellNotifyAutoReg {
    using Fn = void(*)(FjellTypeRegistry*);
    Fn fn;
    FjellNotifyAutoReg* next;
};

inline FjellNotifyAutoReg*& fjell_notify_auto_reg_head() {
    static FjellNotifyAutoReg* head = nullptr;
    return head;
}

// ── Macro helpers ───────────────────────────────────────────────────────

namespace fjell {
// Grants generated RPC invoke trampolines access to private _impl handlers.
// The generated code calls FjellRpcAccess<T>::invoke(self, args...), which is a
// friend of T (declared in FJELL_COMPONENT) so it can reach a private handler.
template <class T> struct FjellRpcAccess;
} // namespace fjell

/// Declares a class as a script component. Place it on the first line of the
/// class body, with the class's own name as the argument.
///
/// It registers the type with the engine automatically at load time, so the
/// component appears in the editor's Add Component list and can be spawned
/// with `Node3D::add_component<T>()`. No central registration file to edit.
///
/// The macro leaves the class in a `public:` section.
///
/// @param T The class name, spelled exactly as declared.
/// @code
/// class Health : public fjell::ScriptComponent {
///     FJELL_COMPONENT(Health)
/// public:
///     [[fj::property]] int max_hp = 100;
/// };
/// @endcode
#define FJELL_COMPONENT(T) \
    public: static constexpr const char* type_name = #T; \
    template <class _FjAccessT> friend struct ::fjell::FjellRpcAccess; \
    private: \
    static inline FjellAutoReg _fjell_reg_{ \
        [](FjellTypeRegistry* r) { fjell::register_component<T>(r); }, \
        nullptr}; \
    struct _FjellRegInit { _FjellRegInit() { \
        _fjell_reg_.next = fjell_auto_reg_head(); \
        fjell_auto_reg_head() = &_fjell_reg_; \
    }}; \
    static inline _FjellRegInit _fjell_reg_init_{}; \
    public:

/// Declares a class as an animation notify. Place it on the first line of the
/// class body of an `AnimNotify` or `AnimNotifyState` subclass.
///
/// Registers the type so it can be placed on animation timelines in the editor.
/// The macro leaves the class in a `public:` section.
///
/// @param T The class name, spelled exactly as declared.
/// @code
/// struct FootstepNotify : fjell::AnimNotify {
///     FJELL_NOTIFY(FootstepNotify)
///     void notify(fjell::Node3D& owner) override { /* ... */ }
/// };
/// @endcode
#define FJELL_NOTIFY(T) \
    public: static constexpr const char* type_name = #T; \
    [[nodiscard]] const char* notify_type_name() const override { return #T; } \
    private: \
    static inline FjellNotifyAutoReg _fjell_notify_reg_{ \
        [](FjellTypeRegistry* r) { fjell::register_notify<T>(r); }, \
        nullptr}; \
    struct _FjellNotifyRegInit { _FjellNotifyRegInit() { \
        _fjell_notify_reg_.next = fjell_notify_auto_reg_head(); \
        fjell_notify_auto_reg_head() = &_fjell_notify_reg_; \
    }}; \
    static inline _FjellNotifyRegInit _fjell_notify_reg_init_{}; \
    public:

/// Emits the game module's entry points. Put it exactly once per project, at
/// file scope in any one `.cpp` — conventionally a small `register.cpp`.
///
/// It publishes the SDK version the module was built against and hands the
/// engine every type declared with `FJELL_COMPONENT`, `FJELL_UI_WIDGET`, or
/// `FJELL_NOTIFY`. Individual types do not need listing; the macros collect
/// themselves.
///
/// Without it the module loads but registers nothing, and the editor shows no
/// components. Used more than once, the build fails with duplicate symbols.
///
/// @code
/// // register.cpp
/// #include "fjell/registry.hpp"
/// FJELL_REGISTER()
/// @endcode
#define FJELL_REGISTER() \
    extern "C" FJELL_GAME_EXPORT uint32_t fjell_api_version() { return FJELL_API_VERSION; } \
    extern "C" FJELL_GAME_EXPORT void fjell_register(FjellTypeRegistry* registry) { \
        for (auto* r = fjell_auto_reg_head(); r; r = r->next) r->fn(registry); \
        for (auto* r = fjell_ui_auto_reg_head(); r; r = r->next) r->fn(registry); \
        for (auto* r = fjell_notify_auto_reg_head(); r; r = r->next) r->fn(registry); \
    }
