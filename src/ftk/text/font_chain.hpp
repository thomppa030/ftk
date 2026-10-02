#pragma once

// The faces text is drawn from, in order: a character is drawn by the first
// face that has it. The theme's face comes first, then faces for what it
// lacks, such as the icon font.
//
//     text::FontChain chain({inter, icons});
//     const text::Face* face = chain.face_for(U'日');

#include "ftk/text/face.hpp"

#include <memory>
#include <vector>

namespace ftk::text {

class FontChain {
public:
    FontChain() = default;
    /// `faces` in the order they are tried; the first is the primary face,
    /// whose metrics a line takes.
    explicit FontChain(std::vector<std::shared_ptr<const Face>> faces);

    [[nodiscard]] const std::vector<std::shared_ptr<const Face>>& faces() const { return faces_; }
    [[nodiscard]] bool empty() const { return faces_.empty(); }

    /// The face that draws `codepoint`: the first that has it, or the
    /// primary face when none does, which draws its missing-glyph box.
    [[nodiscard]] const Face& face_for(char32_t codepoint) const;

    /// The first face.
    [[nodiscard]] const Face& primary() const { return *faces_.front(); }

private:
    std::vector<std::shared_ptr<const Face>> faces_;
};

} // namespace ftk::text
