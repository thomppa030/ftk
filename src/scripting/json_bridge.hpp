#pragma once

#include <nlohmann/json.hpp>

// Engine-side wrappers around the opaque FjellJsonWriter/FjellJsonReader handles.
// These types are what the C API's opaque pointers actually point to.

namespace fjell {

struct JsonWriterImpl {
    nlohmann::json* j;
};

struct JsonReaderImpl {
    const nlohmann::json* j;
};

} // namespace fjell
