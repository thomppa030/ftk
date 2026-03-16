#include "scripting/json_bridge.hpp"
#include "scripting/fjell_api.h"

#include <cstring>

// The FjellJsonWriter/FjellJsonReader opaque types map to our impl structs.
// The C API functions cast the opaque pointer to the engine-side wrapper.

static auto* w_impl(FjellJsonWriter* w) {
    return reinterpret_cast<fjell::JsonWriterImpl*>(w);
}

static auto* r_impl(const FjellJsonReader* r) {
    return reinterpret_cast<const fjell::JsonReaderImpl*>(r);
}

extern "C" {

void fjell_json_write_float(FjellJsonWriter* w, const char* key, float v) {
    (*w_impl(w)->j)[key] = v;
}

void fjell_json_write_int(FjellJsonWriter* w, const char* key, int32_t v) {
    (*w_impl(w)->j)[key] = v;
}

void fjell_json_write_bool(FjellJsonWriter* w, const char* key, uint8_t v) {
    (*w_impl(w)->j)[key] = static_cast<bool>(v);
}

void fjell_json_write_vec2(FjellJsonWriter* w, const char* key, const float* xy) {
    (*w_impl(w)->j)[key] = {xy[0], xy[1]};
}

void fjell_json_write_vec3(FjellJsonWriter* w, const char* key, const float* xyz) {
    (*w_impl(w)->j)[key] = {xyz[0], xyz[1], xyz[2]};
}

void fjell_json_write_vec4(FjellJsonWriter* w, const char* key, const float* xyzw) {
    (*w_impl(w)->j)[key] = {xyzw[0], xyzw[1], xyzw[2], xyzw[3]};
}

void fjell_json_write_string(FjellJsonWriter* w, const char* key, const char* v) {
    (*w_impl(w)->j)[key] = v;
}

float fjell_json_read_float(const FjellJsonReader* r, const char* key, float fb) {
    return r_impl(r)->j->value(key, fb);
}

int32_t fjell_json_read_int(const FjellJsonReader* r, const char* key, int32_t fb) {
    return r_impl(r)->j->value(key, fb);
}

uint8_t fjell_json_read_bool(const FjellJsonReader* r, const char* key, uint8_t fb) {
    return r_impl(r)->j->value(key, static_cast<bool>(fb)) ? 1 : 0;
}

void fjell_json_read_vec2(const FjellJsonReader* r, const char* key,
                          float* out, const float* fb) {
    auto& j = *r_impl(r)->j;
    if (j.contains(key) && j[key].is_array() && j[key].size() >= 2) {
        out[0] = j[key][0].get<float>();
        out[1] = j[key][1].get<float>();
    } else {
        std::memcpy(out, fb, sizeof(float) * 2);
    }
}

void fjell_json_read_vec3(const FjellJsonReader* r, const char* key,
                          float* out, const float* fb) {
    auto& j = *r_impl(r)->j;
    if (j.contains(key) && j[key].is_array() && j[key].size() >= 3) {
        out[0] = j[key][0].get<float>();
        out[1] = j[key][1].get<float>();
        out[2] = j[key][2].get<float>();
    } else {
        std::memcpy(out, fb, sizeof(float) * 3);
    }
}

void fjell_json_read_vec4(const FjellJsonReader* r, const char* key,
                          float* out, const float* fb) {
    auto& j = *r_impl(r)->j;
    if (j.contains(key) && j[key].is_array() && j[key].size() >= 4) {
        out[0] = j[key][0].get<float>();
        out[1] = j[key][1].get<float>();
        out[2] = j[key][2].get<float>();
        out[3] = j[key][3].get<float>();
    } else {
        std::memcpy(out, fb, sizeof(float) * 4);
    }
}

const char* fjell_json_read_string(const FjellJsonReader* r, const char* key,
                                   const char* fb) {
    auto& j = *r_impl(r)->j;
    if (j.contains(key) && j[key].is_string()) {
        // Return pointer into the json object's string storage.
        // Valid as long as the json object lives.
        return j[key].get_ref<const std::string&>().c_str();
    }
    return fb;
}

} // extern "C"
