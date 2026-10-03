#include "ftk/ui/core/id.hpp"

#include <cstring>

namespace ftk::ui::core {

namespace {

// FNV-1a over the seed's bytes, then the data's: short labels spread well,
// and a different seed gives a different id for the same label.
constexpr uint32_t FNV_OFFSET = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

uint32_t mix(uint32_t hash, const unsigned char* bytes, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

Id hash_bytes(const void* data, std::size_t size, Id seed) {
    unsigned char seed_bytes[sizeof(seed)];
    std::memcpy(seed_bytes, &seed, sizeof(seed));
    uint32_t hash = mix(FNV_OFFSET, seed_bytes, sizeof(seed));
    hash = mix(hash, static_cast<const unsigned char*>(data), size);
    return hash == 0 ? 1 : hash;
}

} // namespace

Id hash_label(std::string_view label, Id seed) {
    // From "###" on, alone: the label before it can change.
    if (const std::size_t fixed = label.rfind("###"); fixed != std::string_view::npos) label.remove_prefix(fixed);
    return hash_bytes(label.data(), label.size(), seed);
}

Id hash_int(int value, Id seed) {
    return hash_bytes(&value, sizeof(value), seed);
}

Id hash_pointer(const void* pointer, Id seed) {
    return hash_bytes(&pointer, sizeof(pointer), seed);
}

std::string_view shown_label(std::string_view label) {
    return label.substr(0, label.find("##"));
}

} // namespace ftk::ui::core
