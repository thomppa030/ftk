#pragma once

// Code shown rather than edited: what an editor will write, a file's part
// that a panel is about. In the mono face on the sunken surface, each
// stretch in the code colour of what it is (sheet 39, K1), filling the room
// there is and scrolling when it holds more.
//
//     std::vector<ui::CodeSpan> spans{{.begin = 0, .end = 5, .kind = ui::CodeKind::keyword}};
//     ui::code_view("##source", "panel {}", spans);

#include "ftk/ui/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ftk::ui {

/// What a stretch of code is, as the theme colours it.
enum class CodeKind : uint8_t {
    text,        ///< a plain name, and what no span covers
    keyword,     ///< a language's own words, a markup tag
    type,        ///< a type, a style sheet's selector
    function,
    string,
    number,
    comment,
    meta,        ///< what speaks to the reader of the file: #include, a binding
    punctuation,
    member,      ///< a field, an attribute, a property
    builtin,     ///< the names an engine gives
};

/// A stretch of a code view's text, in bytes, and what it is.
struct CodeSpan {
    std::size_t begin{0};
    std::size_t end{0};
    CodeKind kind{CodeKind::text};
};

/// The colour code of `kind` is drawn in.
[[nodiscard]] Color code_colour(CodeKind kind);

/// `text` under `id`, read only, filling the room there is: 12.5 px mono
/// on 18 px lines, a tab four spaces wide. `spans` say what each stretch
/// is, in order and none overlapping; one may run over lines.
void code_view(const char* id, std::string_view text, std::span<const CodeSpan> spans);

} // namespace ftk::ui
