#include "ftk/text/shape.hpp"

#include "ftk/base/utf8.hpp"
#include "ftk/text/detail/face_impl.hpp"

namespace ftk::text {

namespace {

// A stretch of the text drawn by one face in one script, shaped as one.
struct Run {
    std::size_t begin{0};
    std::size_t end{0};
    const Face* face{nullptr};
    hb_script_t script{HB_SCRIPT_COMMON};
};

// Scripts that take the script of the text around them: spaces, digits and
// punctuation, and marks.
bool neutral(hb_script_t script) {
    return script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED || script == HB_SCRIPT_UNKNOWN;
}

// A character drawn with the one before it: a combining mark, a joiner or a
// variation selector. It goes in its base's run, whatever face has it.
bool joins_previous(hb_unicode_funcs_t* unicode, char32_t codepoint) {
    switch (hb_unicode_general_category(unicode, codepoint)) {
        case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
        case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
        case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
            return true;
        default:
            break;
    }
    return codepoint == 0x200C || codepoint == 0x200D || (codepoint >= 0xFE00 && codepoint <= 0xFE0F) ||
           (codepoint >= 0xE0100 && codepoint <= 0xE01EF);
}

// Splits `text` where the face that draws it or its script changes. A space
// or a punctuation mark the run's face has stays in the run, so a line of
// ideographs drawn by a fallback face keeps that face's spaces; a character
// another face claims never does, and the last resort keeps none, since it
// has every character only to name what is missing.
std::vector<Run> itemize(std::string_view text, const FontChain& chain, hb_unicode_funcs_t* unicode) {
    std::vector<Run> runs;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t at = i;
        const auto codepoint = static_cast<char32_t>(utf8::next(text, i));
        const hb_script_t script = hb_unicode_script(unicode, codepoint);
        const Face* claimed = chain.claimant(codepoint);
        if (!runs.empty() && (claimed == nullptr || claimed == runs.back().face)) {
            Run& run = runs.back();
            const bool keeps_neutral = run.face != chain.last_resort() && run.face->covers(codepoint);
            if (joins_previous(unicode, codepoint) || (neutral(script) && keeps_neutral)) {
                run.end = i;
                continue;
            }
        }
        const Face* face = claimed != nullptr ? claimed : &chain.face_for(codepoint);
        if (!runs.empty()) {
            Run& run = runs.back();
            const bool same_script = neutral(script) || neutral(run.script) || run.script == script;
            if (run.face == face && same_script) {
                if (neutral(run.script)) run.script = script;
                run.end = i;
                continue;
            }
        }
        runs.push_back({.begin = at, .end = i, .face = face, .script = script});
    }
    return runs;
}

} // namespace

ShapedText shape(std::string_view text, const FontChain& chain, float size, const ShapeOptions& options) {
    ShapedText shaped;
    if (text.empty() || chain.empty()) return shaped;

    hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();
    const hb_language_t language =
        options.language.empty()
            ? HB_LANGUAGE_INVALID
            : hb_language_from_string(options.language.data(), static_cast<int>(options.language.size()));
    hb_buffer_t* buffer = hb_buffer_create();
    float pen = 0.0f;
    for (const Run& run : itemize(text, chain, unicode)) {
        hb_buffer_clear_contents(buffer);
        // The whole text goes in, the run marked out of it: shaping sees the
        // characters on either side, and clusters are bytes of the whole.
        hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), static_cast<unsigned int>(run.begin),
                           static_cast<int>(run.end - run.begin));
        hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
        if (!neutral(run.script)) hb_buffer_set_script(buffer, run.script);
        if (language != HB_LANGUAGE_INVALID) hb_buffer_set_language(buffer, language);
        hb_buffer_guess_segment_properties(buffer);
        hb_shape(run.face->impl().hb, buffer, nullptr, 0);

        unsigned int count = 0;
        const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
        const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
        // Shaped in font units; HarfBuzz's y grows up, the screen's down.
        const float scale = size / run.face->units_per_em();
        for (unsigned int k = 0; k < count; ++k) {
            const float advance = static_cast<float>(positions[k].x_advance) * scale;
            shaped.glyphs.push_back({.face = run.face,
                                     .index = infos[k].codepoint,
                                     .cluster = infos[k].cluster,
                                     .x = pen,
                                     .offset = {static_cast<float>(positions[k].x_offset) * scale,
                                                -static_cast<float>(positions[k].y_offset) * scale},
                                     .advance = advance});
            pen += advance;
        }
    }
    hb_buffer_destroy(buffer);
    shaped.width = pen;
    return shaped;
}

} // namespace ftk::text
