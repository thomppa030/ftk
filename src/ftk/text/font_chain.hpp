#pragma once

// The faces text is drawn from, in order: a character is drawn by the first
// face that has it. The theme's face comes first, then faces for what it
// lacks, such as the icon font, then the system's fallback fonts, and last
// the face of last resort (ftk/text/last_resort.hpp).
//
//     text::FontChain chain({inter, icons}, std::make_shared<text::SystemFallback>(), text::last_resort());
//     const text::Face& face = chain.face_for(U'日');

#include "ftk/text/face.hpp"
#include "ftk/text/system_fallback.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace ftk::text {

/// How the glyphs of a claimed face sit on the line.
enum class ClaimSeat : uint8_t {
    /// On the baseline, as text's do.
    Baseline,
    /// Their face's middle on the primary face's, as icons sit: an icon
    /// font draws in the em above its baseline, higher than text's middle.
    Centred,
};

class FontChain {
public:
    FontChain() = default;
    /// `faces` in the order they are tried; the first is the primary face,
    /// whose metrics a line takes. Then `system`'s fonts, when given, which
    /// chains may share, and then `last_resort`, when given.
    explicit FontChain(std::vector<std::shared_ptr<const Face>> faces,
                       std::shared_ptr<SystemFallback> system = nullptr,
                       std::shared_ptr<const Face> last_resort = nullptr);

    /// Gives `face` the codepoints `first` to `last`, ahead of every other
    /// face: an icon font's private-use range, which a text face may use
    /// for glyphs of its own (Inter has some 700 there). Its glyphs sit as
    /// `seat` says.
    FontChain& claim(std::shared_ptr<const Face> face, char32_t first, char32_t last,
                     ClaimSeat seat = ClaimSeat::Baseline);

    [[nodiscard]] const std::vector<std::shared_ptr<const Face>>& faces() const { return faces_; }
    [[nodiscard]] bool empty() const { return faces_.empty(); }

    /// The face that draws `codepoint`: the face that claims it, else the
    /// first that has it, else the system's, else the last resort, else the
    /// primary face, which draws its missing-glyph box.
    [[nodiscard]] const Face& face_for(char32_t codepoint) const;

    /// The face that claims `codepoint`, or null when none does.
    [[nodiscard]] const Face* claimant(char32_t codepoint) const;

    /// How far below the baseline the glyphs of `face` sit at `size`: for a
    /// face claimed centred, from its middle down to the primary face's;
    /// zero for any other.
    [[nodiscard]] float drop(const Face& face, float size) const;

    /// The face of last resort, or null when the chain has none.
    [[nodiscard]] const Face* last_resort() const { return last_resort_.get(); }

    /// The first face.
    [[nodiscard]] const Face& primary() const { return *faces_.front(); }

private:
    struct Claim {
        std::shared_ptr<const Face> face;
        char32_t first;
        char32_t last;
        ClaimSeat seat;
    };

    std::vector<std::shared_ptr<const Face>> faces_;
    std::vector<Claim> claims_;
    std::shared_ptr<SystemFallback> system_;
    std::shared_ptr<const Face> last_resort_;
};

} // namespace ftk::text
