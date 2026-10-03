// What a search matches: the same for the kit drawn by either
// implementation, so it is in ftk-ui-kit.

#include "ftk/ui/kit/search.hpp"

#include <algorithm>
#include <cctype>

namespace ftk::ui {

std::size_t detail::find_match(std::string_view text, std::string_view query) {
    if (query.empty()) return 0;
    auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    const auto found = std::ranges::search(text, query, [&](char a, char b) { return lower(a) == lower(b); });
    return found.empty() ? std::string_view::npos : static_cast<std::size_t>(found.begin() - text.begin());
}

bool matches(std::string_view text, std::string_view query) {
    return query.empty() || detail::find_match(text, query) != std::string_view::npos;
}

} // namespace ftk::ui
