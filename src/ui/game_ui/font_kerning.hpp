#pragma once

// Pair kerning read from a font file's GPOS table, where OpenType fonts keep
// it: the pair adjustment lookups of the 'kern' feature. FreeType reads only
// the older 'kern' table, which modern fonts no longer carry. This parses
// the file's bytes and nothing else; the font baker is the only caller.

#include <cstdint>
#include <span>
#include <vector>

namespace fjell {

class FontKerning {
public:
    /// Reads the GPOS kerning of a TrueType or OpenType font, or of the first
    /// font in a collection. A font without it, or whose table does not
    /// parse, reads as having none.
    [[nodiscard]] static FontKerning read(std::span<const uint8_t> font_file);

    /// True when the font kerns through GPOS; when it does not, the older
    /// 'kern' table is the only other place kerning can be.
    [[nodiscard]] bool has_pairs() const { return !lookups_.empty(); }

    /// How much the advance between two glyphs, by glyph index, changes, in
    /// font units: the sum over the 'kern' lookups, each giving the value of
    /// its first subtable that covers the pair. 0 when none does. Only the
    /// first glyph's horizontal advance is read, which is how Latin, Greek
    /// and Cyrillic fonts kern; contextual kerning is not read.
    [[nodiscard]] int pair(uint16_t left, uint16_t right) const;

    /// Font units per em, from the 'head' table; 0 when the font has none.
    [[nodiscard]] uint16_t units_per_em() const { return units_per_em_; }

    // Which glyphs a subtable applies to, and where each sits in its lists.
    struct Coverage {
        struct Range {
            uint16_t first{0};
            uint16_t last{0};
            uint16_t index{0};
        };
        std::vector<uint16_t> glyphs; // sorted; a glyph's index is its position
        std::vector<Range> ranges;    // sorted by first

        [[nodiscard]] int index_of(uint16_t glyph) const;
    };

    // The class each glyph belongs to; 0 for every glyph not listed.
    struct ClassDef {
        struct Range {
            uint16_t first{0};
            uint16_t last{0};
            uint16_t value{0};
        };
        uint16_t first{0};
        std::vector<uint16_t> classes; // for glyphs first, first + 1, ...
        std::vector<Range> ranges;     // sorted by first

        [[nodiscard]] uint16_t class_of(uint16_t glyph) const;
    };

    struct Subtable {
        Coverage coverage;
        bool by_class{false};
        // By glyph: for each covered first glyph, the second glyphs it kerns
        // with and by how much, sorted by glyph.
        std::vector<std::vector<std::pair<uint16_t, int16_t>>> pairs;
        // By class: one value per pair of classes.
        ClassDef first_classes;
        ClassDef second_classes;
        uint16_t first_class_count{0};
        uint16_t second_class_count{0};
        std::vector<int16_t> class_pairs;
    };

    struct Lookup {
        std::vector<Subtable> subtables;
    };

private:
    std::vector<Lookup> lookups_;
    uint16_t units_per_em_{0};
};

} // namespace fjell
