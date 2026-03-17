// fjell.hpp — C++ convenience layer for Fjell components
// Include this instead of fjell_api.h for a nicer authoring experience.
// Everything here is header-only and compiles down to the same C structs
// that ScriptManager expects — no runtime cost.

#pragma once

#include "fjell_api.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fjell {

// ── SFINAE helpers for detecting optional methods on T ───────────────────

namespace detail {

template <typename T, typename = void>
struct has_create : std::false_type {};
template <typename T>
struct has_create<T, std::void_t<decltype(std::declval<T>().create(
    std::declval<const FjellEngineApi*>()))>> : std::true_type {};

template <typename T, typename = void>
struct has_destroy : std::false_type {};
template <typename T>
struct has_destroy<T, std::void_t<decltype(std::declval<T>().destroy())>>
    : std::true_type {};

template <typename T, typename = void>
struct has_update : std::false_type {};
template <typename T>
struct has_update<T, std::void_t<decltype(std::declval<T>().update(
    std::declval<const FjellEngineApi*>(), float{}))>> : std::true_type {};

template <typename T, typename = void>
struct has_inspector : std::false_type {};
template <typename T>
struct has_inspector<T, std::void_t<decltype(std::declval<T>().inspector())>>
    : std::true_type {};

template <typename T, typename = void>
struct has_serialize : std::false_type {};
template <typename T>
struct has_serialize<T, std::void_t<decltype(std::declval<const T>().serialize(
    std::declval<FjellJsonWriter*>()))>> : std::true_type {};

template <typename T, typename = void>
struct has_deserialize : std::false_type {};
template <typename T>
struct has_deserialize<T, std::void_t<decltype(std::declval<T>().deserialize(
    std::declval<const FjellJsonReader*>()))>> : std::true_type {};

template <typename T, typename = void>
struct has_fields : std::false_type {};
template <typename T>
struct has_fields<T, std::void_t<decltype(T::reflect())>> : std::true_type {};

// Thunks — the actual C function pointers stored in FjellComponentType.
// They cast void* back to T* and forward to the member function.

template <typename T>
void create_thunk(void* data, const FjellEngineApi* api) {
    static_cast<T*>(data)->create(api);
}

template <typename T>
void destroy_thunk(void* data) {
    static_cast<T*>(data)->destroy();
}

template <typename T>
void update_thunk(void* data, const FjellEngineApi* api, float dt) {
    static_cast<T*>(data)->update(api, dt);
}

template <typename T>
void inspector_thunk(void* data) {
    static_cast<T*>(data)->inspector();
}

template <typename T>
void serialize_thunk(const void* data, FjellJsonWriter* w) {
    static_cast<const T*>(data)->serialize(w);
}

template <typename T>
void deserialize_thunk(void* data, const FjellJsonReader* r) {
    static_cast<T*>(data)->deserialize(r);
}

} // namespace detail

// ── Component<T> CRTP base ──────────────────────────────────────────────
//
// Usage:
//
//   struct Spinner : fjell::Component<Spinner> {
//       static constexpr const char* component_name = "Spinner";
//       float speed = 90.0f;
//
//       // Fields for auto-inspector and serialization
//       static auto reflect() {
//           return std::array{
//               FJELL_F_RANGE(Spinner, speed, FJELL_FIELD_FLOAT, 0, 360, 1),
//           };
//       }
//
//       // Only implement the callbacks you need — all are optional
//       void update(const FjellEngineApi* api, float dt) {
//           float rot[3];
//           api->get_rotation(api->object_id, rot);
//           rot[1] += speed * dt;
//           api->set_rotation(api->object_id, rot);
//       }
//   };
//
//   FJELL_EXPORT(Spinner)

template <typename T>
struct Component {
    static const FjellComponentType* type_desc() {
        static bool built = false;
        static FjellFieldDesc c_fields[64];
        static FjellComponentType desc{};

        if (!built) {
            desc.name = T::component_name;
            desc.instance_size = sizeof(T);
            desc.version = 1;

            if constexpr (detail::has_create<T>::value)
                desc.create = detail::create_thunk<T>;
            if constexpr (detail::has_destroy<T>::value)
                desc.destroy = detail::destroy_thunk<T>;
            if constexpr (detail::has_update<T>::value)
                desc.update = detail::update_thunk<T>;
            if constexpr (detail::has_inspector<T>::value)
                desc.inspector = detail::inspector_thunk<T>;
            if constexpr (detail::has_serialize<T>::value)
                desc.serialize = detail::serialize_thunk<T>;
            if constexpr (detail::has_deserialize<T>::value)
                desc.deserialize = detail::deserialize_thunk<T>;

            if constexpr (detail::has_fields<T>::value) {
                auto arr = T::reflect();
                for (std::size_t i = 0; i < arr.size(); ++i) {
                    c_fields[i] = arr[i];
                }
                desc.fields = c_fields;
                desc.field_count = static_cast<uint32_t>(arr.size());
            }

            built = true;
        }

        return &desc;
    }
};

// ── get<T> — query a component on the current or another object ─────────
//
//   auto* health = fjell::get<Health>(api);
//   auto* health = fjell::get<Health>(api, other_id);

template <typename T>
T* get(const FjellEngineApi* api) {
    return static_cast<T*>(api->get_component(api->object_id, T::component_name));
}

template <typename T>
T* get(const FjellEngineApi* api, uint32_t object_id) {
    return static_cast<T*>(api->get_component(object_id, T::component_name));
}

// ── Export helper ────────────────────────────────────────────────────────

namespace detail {

template <typename... Ts>
struct TypeList {
    static const FjellComponentType* const* get() {
        static const FjellComponentType* types[] = {Ts::type_desc()..., nullptr};
        return types;
    }
};

} // namespace detail

} // namespace fjell

// ── Field descriptor macros ─────────────────────────────────────────────
// Used inside reflect(). These expand to FjellFieldDesc initializers.

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

// ── Export macro ────────────────────────────────────────────────────────
//   FJELL_EXPORT(Spinner, Health, PlayerController)

#define FJELL_EXPORT(...) \
    extern "C" const FjellComponentType* const* fjell_register_components(void) { \
        return fjell::detail::TypeList<__VA_ARGS__>::get(); \
    }
