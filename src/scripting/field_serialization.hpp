#pragma once

#include "scripting/script_manager.hpp" // OwnedFieldDesc
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fjell {

// Serialize reflected fields to JSON. Handles value types (float, int, bool,
// vec2/3/4, color3/4, string, enum). Ref types are skipped — use the full
// ScriptManager serializer for components with object references.
inline void serialize_reflected_fields(const std::vector<OwnedFieldDesc>& fields,
                                       const void* data, nlohmann::json& out) {
    const auto* base = static_cast<const uint8_t*>(data);
    for (const auto& f : fields) {
        const void* ptr = base + f.offset;
        switch (f.type) {
            case FJELL_FIELD_FLOAT:
                out[f.name] = *static_cast<const float*>(ptr);
                break;
            case FJELL_FIELD_INT:
            case FJELL_FIELD_ENUM:
                out[f.name] = *static_cast<const int32_t*>(ptr);
                break;
            case FJELL_FIELD_BOOL:
                out[f.name] = *static_cast<const bool*>(ptr);
                break;
            case FJELL_FIELD_VEC2: {
                const auto* v = static_cast<const float*>(ptr);
                out[f.name] = {v[0], v[1]};
                break;
            }
            case FJELL_FIELD_VEC3:
            case FJELL_FIELD_COLOR3: {
                const auto* v = static_cast<const float*>(ptr);
                out[f.name] = {v[0], v[1], v[2]};
                break;
            }
            case FJELL_FIELD_VEC4:
            case FJELL_FIELD_COLOR4: {
                const auto* v = static_cast<const float*>(ptr);
                out[f.name] = {v[0], v[1], v[2], v[3]};
                break;
            }
            case FJELL_FIELD_STRING:
                out[f.name] = static_cast<const char*>(ptr);
                break;
            default:
                break; // Ref types not handled here
        }
    }
}

// Deserialize reflected fields from JSON. Same value-type subset.
inline void deserialize_reflected_fields(const std::vector<OwnedFieldDesc>& fields,
                                          void* data, const nlohmann::json& in) {
    auto* base = static_cast<uint8_t*>(data);
    for (const auto& f : fields) {
        void* ptr = base + f.offset;
        if (!in.contains(f.name)) continue;

        switch (f.type) {
            case FJELL_FIELD_FLOAT:
                *static_cast<float*>(ptr) = in[f.name].get<float>();
                break;
            case FJELL_FIELD_INT:
                *static_cast<int32_t*>(ptr) = in[f.name].get<int32_t>();
                break;
            case FJELL_FIELD_ENUM: {
                int32_t val = in[f.name].get<int32_t>();
                int32_t max_val = static_cast<int32_t>(f.size) - 1;
                if (max_val >= 0) val = std::clamp(val, int32_t{0}, max_val);
                *static_cast<int32_t*>(ptr) = val;
                break;
            }
            case FJELL_FIELD_BOOL:
                *static_cast<bool*>(ptr) = in[f.name].get<bool>();
                break;
            case FJELL_FIELD_VEC2: {
                auto* v = static_cast<float*>(ptr);
                auto& a = in[f.name];
                if (a.is_array() && a.size() >= 2) { v[0] = a[0]; v[1] = a[1]; }
                break;
            }
            case FJELL_FIELD_VEC3:
            case FJELL_FIELD_COLOR3: {
                auto* v = static_cast<float*>(ptr);
                auto& a = in[f.name];
                if (a.is_array() && a.size() >= 3) { v[0] = a[0]; v[1] = a[1]; v[2] = a[2]; }
                break;
            }
            case FJELL_FIELD_VEC4:
            case FJELL_FIELD_COLOR4: {
                auto* v = static_cast<float*>(ptr);
                auto& a = in[f.name];
                if (a.is_array() && a.size() >= 4) { v[0] = a[0]; v[1] = a[1]; v[2] = a[2]; v[3] = a[3]; }
                break;
            }
            case FJELL_FIELD_STRING: {
                auto s = in[f.name].get<std::string>();
                std::strncpy(static_cast<char*>(ptr), s.c_str(), f.size - 1);
                static_cast<char*>(ptr)[f.size - 1] = '\0';
                break;
            }
            default:
                break; // Ref types not handled here
        }
    }
}

} // namespace fjell
