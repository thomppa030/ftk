#pragma once

#include <cstddef>
#include <cstdio>
#include <ranges>
#include <type_traits>

namespace ftk {

/// A contiguous run of plain values a file holds as their bytes: a vector,
/// an array, a span.
template <class Items>
concept FileItems = std::ranges::contiguous_range<Items> && std::ranges::sized_range<Items> &&
                    std::is_trivially_copyable_v<std::ranges::range_value_t<Items>>;

/// Writes `items` to `file` as their bytes; true when every one was written.
/// None writes nothing and is true: an empty container's data() may be null,
/// which std::fwrite must not be handed even for no bytes.
template <FileItems Items>
bool write_all(std::FILE* file, const Items& items) {
    const std::size_t count = std::ranges::size(items);
    if (count == 0) return true;
    return std::fwrite(std::ranges::data(items), sizeof(std::ranges::range_value_t<Items>), count, file) == count;
}

/// Reads as many values as `items` holds from `file` into it; true when every
/// one was read. None reads nothing and is true, as write_all() writes none.
template <FileItems Items>
    requires(!std::is_const_v<std::remove_reference_t<std::ranges::range_reference_t<Items>>>)
[[nodiscard]] bool read_all(std::FILE* file, Items&& items) {
    const std::size_t count = std::ranges::size(items);
    if (count == 0) return true;
    return std::fread(std::ranges::data(items), sizeof(std::ranges::range_value_t<Items>), count, file) == count;
}

} // namespace ftk
