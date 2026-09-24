#include "ui/game_ui/font_kerning.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <optional>

namespace fjell {

namespace {

constexpr uint32_t tag(const char (&name)[5]) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(name[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(name[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(name[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(name[3]));
}

// Big-endian reads within a span. A read past the end marks the data bad
// and gives 0, so a parse runs to its end and is thrown away once, rather
// than checking every field.
class Bytes {
public:
    explicit Bytes(std::span<const uint8_t> data) : data_{data} {}

    [[nodiscard]] bool fits(size_t at, size_t length) const {
        return at <= data_.size() && data_.size() - at >= length;
    }

    uint16_t u16(size_t at) {
        if (!fits(at, 2)) return fail();
        return static_cast<uint16_t>((data_[at] << 8) | data_[at + 1]);
    }

    int16_t s16(size_t at) { return static_cast<int16_t>(u16(at)); }

    uint32_t u32(size_t at) {
        if (!fits(at, 4)) return fail();
        return (static_cast<uint32_t>(data_[at]) << 24) | (static_cast<uint32_t>(data_[at + 1]) << 16) |
               (static_cast<uint32_t>(data_[at + 2]) << 8) | static_cast<uint32_t>(data_[at + 3]);
    }

    // Marks the data bad; returns 0 so a read can end with it.
    uint16_t fail() {
        bad_ = true;
        return 0;
    }

    [[nodiscard]] bool bad() const { return bad_; }

private:
    std::span<const uint8_t> data_;
    bool bad_{false};
};

// One table of the font by its tag, or of the first font of a collection.
// Empty when the font has no such table or it lies outside the file.
std::span<const uint8_t> find_table(std::span<const uint8_t> file, uint32_t wanted) {
    Bytes bytes(file);
    size_t font = 0;
    if (bytes.u32(0) == tag("ttcf")) font = bytes.u32(12);
    const uint16_t count = bytes.u16(font + 4);
    for (uint16_t i = 0; i < count && !bytes.bad(); ++i) {
        const size_t record = font + 12 + size_t{16} * i;
        if (bytes.u32(record) != wanted) continue;
        const uint32_t offset = bytes.u32(record + 8);
        const uint32_t length = bytes.u32(record + 12);
        if (bytes.bad() || !bytes.fits(offset, length)) return {};
        return file.subspan(offset, length);
    }
    return {};
}

// A value record holds one 16-bit field for each of the eight low bits set
// in its format, in bit order.
size_t value_record_size(uint16_t format) {
    return size_t{2} * static_cast<size_t>(std::popcount(static_cast<unsigned>(format & 0xFF)));
}

// Where the horizontal advance sits in a value record, if it is there.
std::optional<size_t> x_advance_offset(uint16_t format) {
    constexpr uint16_t X_ADVANCE = 0x0004;
    if ((format & X_ADVANCE) == 0) return std::nullopt;
    return size_t{2} * static_cast<size_t>(std::popcount(static_cast<unsigned>(format & (X_ADVANCE - 1))));
}

FontKerning::Coverage read_coverage(Bytes& bytes, size_t at) {
    FontKerning::Coverage coverage;
    const uint16_t format = bytes.u16(at);
    const uint16_t count = bytes.u16(at + 2);
    if (format == 1) {
        if (!bytes.fits(at + 4, size_t{2} * count)) {
            bytes.fail();
            return coverage;
        }
        coverage.glyphs.reserve(count);
        for (uint16_t i = 0; i < count; ++i) coverage.glyphs.push_back(bytes.u16(at + 4 + size_t{2} * i));
        if (!std::is_sorted(coverage.glyphs.begin(), coverage.glyphs.end())) bytes.fail();
    } else if (format == 2) {
        if (!bytes.fits(at + 4, size_t{6} * count)) {
            bytes.fail();
            return coverage;
        }
        coverage.ranges.reserve(count);
        for (uint16_t i = 0; i < count; ++i) {
            const size_t record = at + 4 + size_t{6} * i;
            coverage.ranges.push_back({bytes.u16(record), bytes.u16(record + 2), bytes.u16(record + 4)});
        }
        if (!std::is_sorted(coverage.ranges.begin(), coverage.ranges.end(),
                            [](const auto& a, const auto& b) { return a.first < b.first; })) {
            bytes.fail();
        }
    } else {
        bytes.fail();
    }
    return coverage;
}

FontKerning::ClassDef read_class_def(Bytes& bytes, size_t at) {
    FontKerning::ClassDef classes;
    const uint16_t format = bytes.u16(at);
    if (format == 1) {
        classes.first = bytes.u16(at + 2);
        const uint16_t count = bytes.u16(at + 4);
        if (!bytes.fits(at + 6, size_t{2} * count)) {
            bytes.fail();
            return classes;
        }
        classes.classes.reserve(count);
        for (uint16_t i = 0; i < count; ++i) classes.classes.push_back(bytes.u16(at + 6 + size_t{2} * i));
    } else if (format == 2) {
        const uint16_t count = bytes.u16(at + 2);
        if (!bytes.fits(at + 4, size_t{6} * count)) {
            bytes.fail();
            return classes;
        }
        classes.ranges.reserve(count);
        for (uint16_t i = 0; i < count; ++i) {
            const size_t record = at + 4 + size_t{6} * i;
            classes.ranges.push_back({bytes.u16(record), bytes.u16(record + 2), bytes.u16(record + 4)});
        }
        std::sort(classes.ranges.begin(), classes.ranges.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    } else {
        bytes.fail();
    }
    return classes;
}

// A pair adjustment subtable: kerning listed by glyph (format 1) or by class
// (format 2).
FontKerning::Subtable read_pair_subtable(Bytes& bytes, size_t at) {
    FontKerning::Subtable subtable;
    const uint16_t format = bytes.u16(at);
    subtable.coverage = read_coverage(bytes, at + bytes.u16(at + 2));
    const uint16_t first_format = bytes.u16(at + 4);
    const uint16_t second_format = bytes.u16(at + 6);
    const size_t values = value_record_size(first_format) + value_record_size(second_format);
    const std::optional<size_t> advance = x_advance_offset(first_format);

    if (format == 1) {
        const uint16_t set_count = bytes.u16(at + 8);
        subtable.pairs.resize(set_count);
        for (uint16_t s = 0; s < set_count && !bytes.bad(); ++s) {
            const size_t set = at + bytes.u16(at + 10 + size_t{2} * s);
            const uint16_t count = bytes.u16(set);
            const size_t record_size = 2 + values;
            if (!bytes.fits(set + 2, record_size * count)) {
                bytes.fail();
                break;
            }
            auto& list = subtable.pairs[s];
            list.reserve(count);
            for (uint16_t k = 0; k < count; ++k) {
                const size_t record = set + 2 + record_size * k;
                list.emplace_back(bytes.u16(record), advance ? bytes.s16(record + 2 + *advance) : int16_t{0});
            }
            std::stable_sort(list.begin(), list.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
        }
    } else if (format == 2) {
        subtable.by_class = true;
        subtable.first_classes = read_class_def(bytes, at + bytes.u16(at + 8));
        subtable.second_classes = read_class_def(bytes, at + bytes.u16(at + 10));
        subtable.first_class_count = bytes.u16(at + 12);
        subtable.second_class_count = bytes.u16(at + 14);
        const size_t cells = size_t{subtable.first_class_count} * subtable.second_class_count;
        // Checked before anything is allocated: the counts could ask for
        // gigabytes that the table does not hold.
        if (!bytes.fits(at + 16, cells * values)) {
            bytes.fail();
            return subtable;
        }
        subtable.class_pairs.assign(cells, 0);
        if (advance) {
            for (size_t cell = 0; cell < cells; ++cell) {
                subtable.class_pairs[cell] = bytes.s16(at + 16 + cell * values + *advance);
            }
        }
    } else {
        bytes.fail();
    }
    return subtable;
}

// The subtables of one lookup that adjust pairs, found directly or through
// an extension, which only moves a subtable further away.
FontKerning::Lookup read_lookup(Bytes& bytes, size_t at) {
    constexpr uint16_t PAIR_ADJUSTMENT = 2;
    constexpr uint16_t EXTENSION = 9;
    FontKerning::Lookup lookup;
    const uint16_t type = bytes.u16(at);
    const uint16_t count = bytes.u16(at + 4);
    for (uint16_t i = 0; i < count && !bytes.bad(); ++i) {
        size_t subtable = at + bytes.u16(at + 6 + size_t{2} * i);
        uint16_t subtable_type = type;
        if (type == EXTENSION) {
            subtable_type = bytes.u16(subtable + 2);
            subtable += bytes.u32(subtable + 4);
        }
        if (subtable_type == PAIR_ADJUSTMENT) lookup.subtables.push_back(read_pair_subtable(bytes, subtable));
    }
    return lookup;
}

} // namespace

int FontKerning::Coverage::index_of(uint16_t glyph) const {
    if (!glyphs.empty()) {
        auto it = std::lower_bound(glyphs.begin(), glyphs.end(), glyph);
        return it != glyphs.end() && *it == glyph ? static_cast<int>(it - glyphs.begin()) : -1;
    }
    auto it = std::upper_bound(ranges.begin(), ranges.end(), glyph,
                               [](uint16_t g, const Range& range) { return g < range.first; });
    if (it == ranges.begin()) return -1;
    --it;
    if (glyph > it->last) return -1;
    return it->index + (glyph - it->first);
}

uint16_t FontKerning::ClassDef::class_of(uint16_t glyph) const {
    if (glyph >= first && static_cast<size_t>(glyph - first) < classes.size()) return classes[glyph - first];
    auto it = std::upper_bound(ranges.begin(), ranges.end(), glyph,
                               [](uint16_t g, const Range& range) { return g < range.first; });
    if (it == ranges.begin()) return 0;
    --it;
    return glyph <= it->last ? it->value : 0;
}

FontKerning FontKerning::read(std::span<const uint8_t> font_file) {
    FontKerning kerning;
    const auto head = find_table(font_file, tag("head"));
    if (Bytes head_bytes(head); head.size() >= 20) kerning.units_per_em_ = head_bytes.u16(18);

    const auto table = find_table(font_file, tag("GPOS"));
    if (table.empty()) return kerning;
    Bytes gpos(table);
    const size_t features = gpos.u16(6);
    const size_t lookups = gpos.u16(8);

    // Every lookup a 'kern' feature names, under any script, each once and
    // in the order the font lists them, which is the order they apply in.
    std::vector<uint16_t> kern_lookups;
    const uint16_t feature_count = gpos.u16(features);
    for (uint16_t f = 0; f < feature_count && !gpos.bad(); ++f) {
        const size_t record = features + 2 + size_t{6} * f;
        if (gpos.u32(record) != tag("kern")) continue;
        const size_t feature = features + gpos.u16(record + 4);
        const uint16_t count = gpos.u16(feature + 2);
        for (uint16_t i = 0; i < count; ++i) kern_lookups.push_back(gpos.u16(feature + 4 + size_t{2} * i));
    }
    std::sort(kern_lookups.begin(), kern_lookups.end());
    kern_lookups.erase(std::unique(kern_lookups.begin(), kern_lookups.end()), kern_lookups.end());

    std::vector<Lookup> read_lookups;
    const uint16_t lookup_count = gpos.u16(lookups);
    for (const uint16_t index : kern_lookups) {
        if (index >= lookup_count || gpos.bad()) break;
        Lookup lookup = read_lookup(gpos, lookups + gpos.u16(lookups + 2 + size_t{2} * index));
        if (!lookup.subtables.empty()) read_lookups.push_back(std::move(lookup));
    }
    if (!gpos.bad()) kerning.lookups_ = std::move(read_lookups);
    return kerning;
}

int FontKerning::pair(uint16_t left, uint16_t right) const {
    int total = 0;
    for (const auto& lookup : lookups_) {
        // The first subtable that covers the pair gives the lookup's value.
        for (const auto& subtable : lookup.subtables) {
            const int index = subtable.coverage.index_of(left);
            if (index < 0) continue;
            if (subtable.by_class) {
                const uint16_t first = subtable.first_classes.class_of(left);
                const uint16_t second = subtable.second_classes.class_of(right);
                if (first >= subtable.first_class_count || second >= subtable.second_class_count) continue;
                total += subtable.class_pairs[size_t{first} * subtable.second_class_count + second];
                break;
            }
            if (static_cast<size_t>(index) >= subtable.pairs.size()) continue;
            const auto& list = subtable.pairs[static_cast<size_t>(index)];
            auto it = std::lower_bound(list.begin(), list.end(), right,
                                       [](const auto& entry, uint16_t glyph) { return entry.first < glyph; });
            if (it == list.end() || it->first != right) continue;
            total += it->second;
            break;
        }
    }
    return total;
}

} // namespace fjell
