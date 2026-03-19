// fjell_registry.hpp — Type registration for Fjell game .so files
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include "fjell_actor.hpp"
#include "fjell_component.hpp"
#include "fjell_api.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

// ── Type descriptors (C-compatible structs) ─────────────────────────────

struct FjellActorType {
    const char* name;
    fjell::Actor* (*create)();
    const FjellFieldDesc* fields;
    uint32_t field_count;
};

struct FjellComponentTypeOOP {
    const char* name;
    fjell::ScriptComponent* (*create)();
    const FjellFieldDesc* fields;
    uint32_t field_count;
};

// ── Opaque registry handle (engine-side) ────────────────────────────────

struct FjellTypeRegistry;

// Engine exports these via -rdynamic — game .so calls them at registration
extern "C" void fjell_reg_actor(FjellTypeRegistry* reg, const FjellActorType* desc);
extern "C" void fjell_reg_component(FjellTypeRegistry* reg, const FjellComponentTypeOOP* desc);

namespace fjell {

// ── SFINAE helper for detecting reflect() ───────────────────────────────

namespace detail {

template <typename T, typename = void>
struct has_reflect : std::false_type {};
template <typename T>
struct has_reflect<T, std::void_t<decltype(T::reflect())>> : std::true_type {};

} // namespace detail

// ── Registration helpers (called from game code) ────────────────────────

template <typename T>
void register_actor(FjellTypeRegistry* reg) {
    static FjellFieldDesc fields[64]{};
    static FjellActorType desc{};

    desc.name = T::type_name;
    desc.create = []() -> Actor* { return new T(); };

    if constexpr (detail::has_reflect<T>::value) {
        auto arr = T::reflect();
        for (std::size_t i = 0; i < arr.size(); ++i) {
            fields[i] = arr[i];
        }
        desc.fields = fields;
        desc.field_count = static_cast<uint32_t>(arr.size());
    }

    fjell_reg_actor(reg, &desc);
}

template <typename T>
void register_component(FjellTypeRegistry* reg) {
    static FjellFieldDesc fields[64]{};
    static FjellComponentTypeOOP desc{};

    desc.name = T::type_name;
    desc.create = []() -> ScriptComponent* { return new T(); };

    if constexpr (detail::has_reflect<T>::value) {
        auto arr = T::reflect();
        for (std::size_t i = 0; i < arr.size(); ++i) {
            fields[i] = arr[i];
        }
        desc.fields = fields;
        desc.field_count = static_cast<uint32_t>(arr.size());
    }

    fjell_reg_component(reg, &desc);
}

} // namespace fjell

// ── Field descriptor macros ─────────────────────────────────────────────

#define FJELL_F(T, member, ftype) \
    FjellFieldDesc { \
        #member, ftype, \
        static_cast<uint32_t>(offsetof(T, member)), \
        static_cast<uint32_t>(sizeof(((T*)nullptr)->member)), \
        nullptr, 0.0f, 0.0f, 0.0f \
    }

#define FJELL_F_RANGE(T, member, ftype, lo, hi, spd) \
    FjellFieldDesc { \
        #member, ftype, \
        static_cast<uint32_t>(offsetof(T, member)), \
        static_cast<uint32_t>(sizeof(((T*)nullptr)->member)), \
        nullptr, static_cast<float>(lo), static_cast<float>(hi), static_cast<float>(spd) \
    }

#define FJELL_F_ENUM(T, member, labels, count) \
    FjellFieldDesc { \
        #member, FJELL_FIELD_ENUM, \
        static_cast<uint32_t>(offsetof(T, member)), \
        count, labels, 0.0f, 0.0f, 0.0f \
    }

// ── Auto-registration ───────────────────────────────────────────────────
// Each FJELL_COMPONENT builds a linked list of registration functions at
// static-init time. fjell_register() walks the list when the engine loads
// the .so. No central register file needed.

struct FjellAutoReg {
    using Fn = void(*)(FjellTypeRegistry*);
    Fn fn;
    FjellAutoReg* next;
};

inline FjellAutoReg*& fjell_auto_reg_head() {
    static FjellAutoReg* head = nullptr;
    return head;
}

// ── Macro helpers ───────────────────────────────────────────────────────

#define FJELL_ACTOR(T) \
    public: static constexpr const char* type_name = #T;

#define FJELL_COMPONENT(T) \
    public: static constexpr const char* type_name = #T; \
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

#define FJELL_REGISTER() \
    extern "C" void fjell_register(FjellTypeRegistry* registry) { \
        for (auto* r = fjell_auto_reg_head(); r; r = r->next) r->fn(registry); \
    }
