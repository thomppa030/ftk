#include "ftk/text/line_break.hpp"

#include "ftk/base/utf8.hpp"

#include <hb.h>

#include <limits>

namespace ftk::text {

namespace {

// UAX #14's line breaking classes, those Latin and CJK text need.
enum class Class : uint8_t {
    BK, CR, LF, NL, SP, ZW, ZWJ, WJ, GL, CM,
    OP, CL, CP, QU, EX, IS, SY, NS, ID, AL, NU, HY, BA, BB, B2, IN, PR, PO,
};

// Small kana, and the kana and halfwidth marks that never begin a line.
bool small_kana(char32_t c) {
    switch (c) {
        case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x3063:
        case 0x3083: case 0x3085: case 0x3087: case 0x308E: case 0x3095: case 0x3096:
        case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9: case 0x30C3:
        case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE: case 0x30F5: case 0x30F6:
            return true;
        default:
            return (c >= 0x31F0 && c <= 0x31FF) || (c >= 0xFF67 && c <= 0xFF70);
    }
}

// The class a character's general category gives, for what the tables
// below don't name.
Class from_category(hb_unicode_funcs_t* unicode, char32_t c, Class otherwise) {
    switch (hb_unicode_general_category(unicode, c)) {
        case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
        case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
        case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
            return Class::CM;
        case HB_UNICODE_GENERAL_CATEGORY_OPEN_PUNCTUATION: return Class::OP;
        case HB_UNICODE_GENERAL_CATEGORY_CLOSE_PUNCTUATION: return Class::CL;
        case HB_UNICODE_GENERAL_CATEGORY_INITIAL_PUNCTUATION:
        case HB_UNICODE_GENERAL_CATEGORY_FINAL_PUNCTUATION:
            return Class::QU;
        case HB_UNICODE_GENERAL_CATEGORY_DECIMAL_NUMBER: return Class::NU;
        case HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR: return Class::BA;
        case HB_UNICODE_GENERAL_CATEGORY_CURRENCY_SYMBOL: return Class::PR;
        default: return otherwise;
    }
}

Class line_class(char32_t c, hb_unicode_funcs_t* unicode) {
    if (c < 0x80) {
        switch (c) {
            case '\n': return Class::LF;
            case '\r': return Class::CR;
            case 0x0B: case 0x0C: return Class::BK;
            case ' ': return Class::SP;
            case '\t': case '|': return Class::BA;
            case '!': case '?': return Class::EX;
            case '"': case '\'': return Class::QU;
            case '(': case '[': case '{': return Class::OP;
            case ')': case ']': return Class::CP;
            case '}': return Class::CL;
            case ',': case '.': case ':': case ';': return Class::IS;
            case '/': return Class::SY;
            case '-': return Class::HY;
            case '$': case '+': case '\\': return Class::PR;
            case '%': return Class::PO;
            default: return c >= '0' && c <= '9' ? Class::NU : Class::AL;
        }
    }
    switch (c) {
        case 0x85: return Class::NL;
        case 0x2028: case 0x2029: return Class::BK;
        case 0x200B: return Class::ZW;
        case 0x200D: return Class::ZWJ;
        case 0x2060: case 0xFEFF: return Class::WJ;
        case 0xA0: case 0x202F: case 0x2007: case 0x2011: case 0x034F: return Class::GL;
        case 0xAD: case 0x2010: case 0x2012: case 0x2013: return Class::BA;
        case 0x2014: return Class::B2;
        case 0x2024: case 0x2025: case 0x2026: return Class::IN;
        case 0xA1: case 0xBF: return Class::OP;
        case 0xA2: case 0xB0: case 0x2030: case 0x2031: case 0x2032: case 0x2033: return Class::PO;
        // CJK punctuation: the ideographic space, the comma and full stop,
        // and the marks that never begin a line.
        case 0x3000: return Class::BA;
        case 0x3001: case 0x3002: return Class::CL;
        case 0x3005: case 0x301C: case 0x303B: case 0x303C: case 0x309B: case 0x309C: case 0x309D:
        case 0x309E: case 0x30A0: case 0x30FB: case 0x30FC: case 0x30FD: case 0x30FE:
            return Class::NS;
        // Fullwidth punctuation.
        case 0xFF01: case 0xFF1F: return Class::EX;
        case 0xFF0C: case 0xFF0E: case 0xFF61: case 0xFF64: return Class::CL;
        case 0xFF1A: case 0xFF1B: case 0xFF65: case 0xFF9E: case 0xFF9F: return Class::NS;
        case 0xFF04: case 0xFFE1: case 0xFFE5: case 0xFFE6: return Class::PR;
        case 0xFF05: case 0xFFE0: return Class::PO;
        default: break;
    }
    if (small_kana(c)) return Class::NS;
    if (c >= 0x4DC0 && c <= 0x4DFF) return Class::AL; // the I Ching's hexagrams
    // Ideographs, kana, Hangul and fullwidth forms break between characters;
    // their brackets and marks take their category's class.
    const bool ideographic = (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
                             (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
                             (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFFEF) ||
                             (c >= 0x20000 && c <= 0x3FFFD);
    return from_category(unicode, c, ideographic ? Class::ID : Class::AL);
}

bool line_end(Class c) {
    return c == Class::BK || c == Class::CR || c == Class::LF || c == Class::NL;
}

// Whether a line may begin at a character of class `next`, after one of
// class `previous`; `before` is the class of the last character before it
// that is not a space. UAX #14's rules, by number.
Break decide(Class previous, Class next, Class before) {
    using enum Class;
    if (previous == BK) return Break::Mandatory;                                     // LB4
    if (previous == CR && next == LF) return Break::None;                            // LB5
    if (previous == CR || previous == LF || previous == NL) return Break::Mandatory;
    if (line_end(next)) return Break::None;                                          // LB6
    if (next == SP || next == ZW) return Break::None;                                // LB7
    if (before == ZW) return Break::Allowed;                                         // LB8
    if (previous == WJ || next == WJ) return Break::None;                            // LB11
    if (previous == GL) return Break::None;                                          // LB12
    if (next == GL && previous != SP && previous != BA && previous != HY) return Break::None; // LB12a
    if (next == CL || next == CP || next == EX || next == IS || next == SY) return Break::None; // LB13
    if (before == OP) return Break::None;                                            // LB14
    if (before == QU && next == OP) return Break::None;                              // LB15
    if ((before == CL || before == CP) && next == NS) return Break::None;            // LB16
    if (before == B2 && next == B2) return Break::None;                              // LB17
    if (previous == SP) return Break::Allowed;                                       // LB18
    if (next == QU || previous == QU) return Break::None;                            // LB19
    if (next == BA || next == HY || next == NS || previous == BB) return Break::None; // LB21
    if (next == IN) return Break::None;                                              // LB22
    if ((previous == AL && next == NU) || (previous == NU && next == AL)) return Break::None; // LB23
    if ((previous == PR && next == ID) || (previous == ID && next == PO)) return Break::None; // LB23a
    if (((previous == PR || previous == PO) && next == AL) ||                        // LB24
        (previous == AL && (next == PR || next == PO))) {
        return Break::None;
    }
    if (((previous == CL || previous == CP || previous == NU) && (next == PO || next == PR)) || // LB25
        ((previous == PO || previous == PR) && (next == OP || next == NU)) ||
        ((previous == HY || previous == IS || previous == NU || previous == SY) && next == NU)) {
        return Break::None;
    }
    if (previous == AL && next == AL) return Break::None;                            // LB28
    if (previous == IS && next == AL) return Break::None;                            // LB29
    if (((previous == AL || previous == NU) && next == OP) ||                        // LB30
        (previous == CP && (next == AL || next == NU))) {
        return Break::None;
    }
    return Break::Allowed;                                                           // LB31
}

// A character's place in the text and its class, a mark's being its base's.
struct Char {
    std::size_t at{0};
    Class cls{Class::AL};
    /// A mark or joiner on the character before: never a line's first.
    bool attached{false};
};

std::vector<Char> classify(std::string_view text, hb_unicode_funcs_t* unicode) {
    std::vector<Char> chars;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t at = i;
        Class cls = line_class(static_cast<char32_t>(utf8::next(text, i)), unicode);
        if (cls == Class::CM || cls == Class::ZWJ) {
            // LB9: a mark takes the class of the character it is on, unless
            // that is a space or a line end; LB10: alone, it is a letter.
            if (!chars.empty() && !line_end(chars.back().cls) && chars.back().cls != Class::SP &&
                chars.back().cls != Class::ZW) {
                chars.push_back({.at = at, .cls = chars.back().cls, .attached = true});
                continue;
            }
            cls = Class::AL;
        }
        chars.push_back({.at = at, .cls = cls});
    }
    return chars;
}

// A character that hangs past a line's end rather than taking room on it:
// a space or a line end.
bool hangs(std::string_view text, std::size_t at, hb_unicode_funcs_t* unicode) {
    const auto c = static_cast<char32_t>(utf8::next(text, at));
    const Class cls = line_class(c, unicode);
    return cls == Class::SP || cls == Class::ZW || line_end(cls) || c == 0x3000;
}

} // namespace

std::vector<Break> break_opportunities(std::string_view text) {
    std::vector<Break> breaks(text.size(), Break::None);
    hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();
    const std::vector<Char> chars = classify(text, unicode);
    Class before = chars.empty() ? Class::AL : chars.front().cls;
    for (std::size_t k = 1; k < chars.size(); ++k) {
        const Class previous = chars[k - 1].cls;
        if (previous != Class::SP) before = previous;
        breaks[chars[k].at] = chars[k].attached ? Break::None : decide(previous, chars[k].cls, before);
    }
    return breaks;
}

std::vector<Line> break_lines(std::string_view text, const ShapedText& shaped, float width) {
    std::vector<Line> lines;
    const std::vector<Glyph>& glyphs = shaped.glyphs;
    if (glyphs.empty()) return lines;

    hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();
    const std::vector<Break> breaks = break_opportunities(text);
    const auto starts_cluster = [&](std::size_t g) { return g == 0 || glyphs[g].cluster != glyphs[g - 1].cluster; };
    const auto hanging = [&](std::size_t g) { return hangs(text, glyphs[g].cluster, unicode); };
    constexpr std::size_t NONE = std::numeric_limits<std::size_t>::max();

    std::size_t start = 0;
    while (start < glyphs.size()) {
        const float origin = glyphs[start].x;
        std::size_t candidate = NONE;
        std::size_t end = glyphs.size();
        for (std::size_t g = start + 1; g < glyphs.size(); ++g) {
            if (starts_cluster(g)) {
                const Break at = breaks[glyphs[g].cluster];
                if (at == Break::Mandatory) {
                    end = g;
                    break;
                }
                if (at == Break::Allowed) candidate = g;
            }
            if (hanging(g) || glyphs[g].x + glyphs[g].advance - origin <= width) continue;
            // Full: back to the last place a line may begin, or else to the
            // start of the character that overflows, keeping at least the
            // first character on the line.
            if (candidate != NONE) {
                end = candidate;
            } else {
                end = g;
                while (end > start && !starts_cluster(end)) --end;
                if (end == start) {
                    end = g + 1;
                    while (end < glyphs.size() && !starts_cluster(end)) ++end;
                }
            }
            break;
        }

        std::size_t drawn = end;
        while (drawn > start && hanging(drawn - 1)) --drawn;
        lines.push_back({.first_glyph = start,
                         .end_glyph = drawn,
                         .begin = glyphs[start].cluster,
                         .end = end < glyphs.size() ? glyphs[end].cluster : text.size(),
                         .width = drawn > start ? glyphs[drawn - 1].x + glyphs[drawn - 1].advance - origin : 0.0f});
        start = end;
    }
    return lines;
}

} // namespace ftk::text
