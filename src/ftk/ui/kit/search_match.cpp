// What a search matches: the same for the kit drawn by either
// implementation, so it is in ftk-ui-kit.

#include "ftk/ui/kit/search.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>

namespace ftk::ui {

namespace {

// A fuzzy match's scores: for each character found, more where it starts
// the text or a word, or follows the one found before it; less for each
// character between two found, and for those before the first.
constexpr int FOUND = 16;
constexpr int STARTS_TEXT = 32;
constexpr int STARTS_WORD = 24;
constexpr int STARTS_CAPITAL = 20;   // a capital after a small letter: "fuzzyMatch"
constexpr int TOGETHER = 16;
constexpr int GAP = 2;
constexpr int LEAD = 1;
constexpr int LEAD_MOST = 8;
constexpr int NONE = std::numeric_limits<int>::min() / 2;

// A character of UTF-8 text: its code point, ASCII letters made small, and
// where its bytes are.
struct Char {
    uint32_t code{0};
    std::size_t begin{0};
    std::size_t end{0};
};

std::vector<Char> chars_of(std::string_view text) {
    std::vector<Char> out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        uint32_t code = lead;
        if (length > 1) {
            code = lead & (0xFFu >> (length + 1));
            for (std::size_t k = 1; k < length && i + k < text.size(); ++k) {
                code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
            }
        }
        const std::size_t end = std::min(i + length, text.size());
        out.push_back({code, i, end});
        i = end;
    }
    return out;
}

// A character as a search compares it, capitals made small: ASCII, the
// Latin letters of Western and Central European languages, Greek and
// Cyrillic.
uint32_t folded(uint32_t code) {
    if (code < 0x80) return static_cast<uint32_t>(std::tolower(static_cast<int>(code)));
    if (code >= 0xC0 && code <= 0xDE && code != 0xD7) return code + 0x20;   // À to Þ
    // Latin Extended-A: each capital before its small letter, the pairs
    // starting on an even code point, then on an odd one, then even again.
    if ((code >= 0x100 && code <= 0x137) || (code >= 0x14A && code <= 0x177)) return code | 1u;
    if ((code >= 0x139 && code <= 0x148) || (code >= 0x179 && code <= 0x17E)) return code + (code & 1u);
    if (code >= 0x391 && code <= 0x3AB && code != 0x3A2) return code + 0x20;   // Greek
    if (code >= 0x410 && code <= 0x42F) return code + 0x20;                     // Cyrillic
    if (code >= 0x400 && code <= 0x40F) return code + 0x50;
    return code;
}

bool separates(uint32_t code) {
    return code == ' ' || code == '_' || code == '-' || code == '.' || code == '/' || code == '\\' || code == ':';
}

bool is_small(uint32_t code) {
    return code >= 'a' && code <= 'z';
}

bool is_capital(uint32_t code) {
    return code >= 'A' && code <= 'Z';
}

} // namespace

std::optional<FuzzyMatch> fuzzy_match(std::string_view text, std::string_view query) {
    if (query.empty()) return FuzzyMatch{};
    // Most texts lack some of the query's characters: told cheaply first,
    // byte by byte, which they must also be in order (any byte of a
    // character past ASCII standing for any other, as its capital may be
    // other bytes).
    const auto same_byte = [](char a, char b) {
        const auto ua = static_cast<unsigned char>(a);
        const auto ub = static_cast<unsigned char>(b);
        if (ua >= 0x80 || ub >= 0x80) return ua >= 0x80 && ub >= 0x80;
        return std::tolower(ua) == std::tolower(ub);
    };
    std::size_t next = 0;
    for (const char c : text) {
        if (next < query.size() && same_byte(c, query[next])) ++next;
    }
    if (next < query.size()) return std::nullopt;

    const std::vector<Char> t = chars_of(text);
    const std::vector<Char> q = chars_of(query);
    const std::size_t n = t.size();
    const std::size_t m = q.size();
    if (m > n) return std::nullopt;

    // What finding a query character at each place in the text is worth.
    std::vector<int> found_at(n);
    for (std::size_t i = 0; i < n; ++i) {
        int score = FOUND;
        if (i == 0) {
            score += STARTS_TEXT;
        } else if (separates(t[i - 1].code)) {
            score += STARTS_WORD;
        } else if (is_small(t[i - 1].code) && is_capital(t[i].code)) {
            score += STARTS_CAPITAL;
        }
        found_at[i] = score;
    }

    // best(j, i): the best score with the query's first j + 1 characters
    // found, the last at i; from(j, i): where the one before it was found.
    std::vector<int> best_table(m * n, NONE);
    std::vector<std::size_t> from_table(m * n, 0);
    const auto best = [&](std::size_t j, std::size_t i) -> int& { return best_table[j * n + i]; };
    const auto from = [&](std::size_t j, std::size_t i) -> std::size_t& { return from_table[j * n + i]; };
    for (std::size_t i = 0; i < n; ++i) {
        if (folded(t[i].code) != folded(q[0].code)) continue;
        best(0, i) = found_at[i] - LEAD * std::min(static_cast<int>(i), LEAD_MOST);
    }
    for (std::size_t j = 1; j < m; ++j) {
        // The best of the places before i - 1 to have found the character
        // before, its gap's cost taken off as if it ran to i: kept as
        // best(j - 1, k) + GAP * k, the largest so far, and where.
        int far = NONE;
        std::size_t far_at = 0;
        for (std::size_t i = j; i < n; ++i) {
            if (i >= 2 && best(j - 1, i - 2) != NONE) {
                const int reach = best(j - 1, i - 2) + GAP * static_cast<int>(i - 2);
                if (reach > far) {
                    far = reach;
                    far_at = i - 2;
                }
            }
            if (folded(t[i].code) != folded(q[j].code)) continue;
            const int together = best(j - 1, i - 1) != NONE ? best(j - 1, i - 1) + TOGETHER : NONE;
            const int apart = far != NONE ? far - GAP * static_cast<int>(i - 1) : NONE;
            if (together == NONE && apart == NONE) continue;
            if (together >= apart) {
                best(j, i) = together + found_at[i];
                from(j, i) = i - 1;
            } else {
                best(j, i) = apart + found_at[i];
                from(j, i) = far_at;
            }
        }
    }

    // The best last place, then back along where each was found.
    std::size_t last = n;
    for (std::size_t i = 0; i < n; ++i) {
        if (best(m - 1, i) != NONE && (last == n || best(m - 1, i) > best(m - 1, last))) last = i;
    }
    if (last == n) return std::nullopt;
    std::vector<std::size_t> places(m);
    places[m - 1] = last;
    for (std::size_t j = m - 1; j > 0; --j) places[j - 1] = from(j, places[j]);

    FuzzyMatch match{.score = best(m - 1, last), .runs = {}};
    for (const std::size_t i : places) {
        if (!match.runs.empty() && match.runs.back().end == t[i].begin) {
            match.runs.back().end = t[i].end;
        } else {
            match.runs.push_back({t[i].begin, t[i].end});
        }
    }
    return match;
}

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
