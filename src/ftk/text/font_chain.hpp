#pragma once

// The faces text is drawn from, in order: a character is drawn by the first
// face that has it. The theme's face comes first, then faces for what it
// lacks, such as the icon font, and last the system's fallback fonts.
//
//     text::FontChain chain({inter, icons}, std::make_shared<text::SystemFallback>());
//     const text::Face& face = chain.face_for(U'日');

#include "ftk/text/face.hpp"
#include "ftk/text/system_fallback.hpp"

#include <memory>
#include <vector>

namespace ftk::text {

class FontChain {
public:
    FontChain() = default;
    /// `faces` in the order they are tried; the first is the primary face,
    /// whose metrics a line takes. Then `system`'s fonts, when given, which
    /// chains may share.
    explicit FontChain(std::vector<std::shared_ptr<const Face>> faces,
                       std::shared_ptr<SystemFallback> system = nullptr);

    /// Gives `face` the codepoints `first` to `last`, ahead of every other
    /// face: an icon font's private-use range, which a text face may use
    /// for glyphs of its own (Inter has some 700 there).
    FontChain& claim(std::shared_ptr<const Face> face, char32_t first, char32_t last);

    [[nodiscard]] const std::vector<std::shared_ptr<const Face>>& faces() const { return faces_; }
    [[nodiscard]] bool empty() const { return faces_.empty(); }

    /// The face that draws `codepoint`: the face that claims it, else the
    /// first that has it, else the system's, else the primary face, which
    /// draws its missing-glyph box.
    [[nodiscard]] const Face& face_for(char32_t codepoint) const;

    /// The face that claims `codepoint`, or null when none does.
    [[nodiscard]] const Face* claimant(char32_t codepoint) const;

    /// The first face.
    [[nodiscard]] const Face& primary() const { return *faces_.front(); }

private:
    struct Claim {
        std::shared_ptr<const Face> face;
        char32_t first;
        char32_t last;
    };

    std::vector<std::shared_ptr<const Face>> faces_;
    std::vector<Claim> claims_;
    std::shared_ptr<SystemFallback> system_;
};

} // namespace ftk::text
