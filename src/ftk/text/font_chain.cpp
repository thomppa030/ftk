#include "ftk/text/font_chain.hpp"

namespace ftk::text {

FontChain::FontChain(std::vector<std::shared_ptr<const Face>> faces, std::shared_ptr<SystemFallback> system,
                     std::shared_ptr<const Face> last_resort)
    : faces_{std::move(faces)}, system_{std::move(system)}, last_resort_{std::move(last_resort)} {}

FontChain& FontChain::claim(std::shared_ptr<const Face> face, char32_t first, char32_t last, ClaimSeat seat) {
    claims_.push_back({.face = std::move(face), .first = first, .last = last, .seat = seat});
    return *this;
}

float FontChain::drop(const Face& face, float size) const {
    for (const Claim& claim : claims_) {
        if (claim.face.get() != &face || claim.seat != ClaimSeat::Centred) continue;
        // A face's middle, above its baseline: halfway between its ascent
        // and its descent.
        const auto middle = [size](const Face& of) {
            const Metrics m = of.metrics(size);
            return (m.ascent - m.descent) * 0.5f;
        };
        return middle(face) - middle(primary());
    }
    return 0.0f;
}

const Face* FontChain::claimant(char32_t codepoint) const {
    for (const Claim& claim : claims_) {
        if (codepoint >= claim.first && codepoint <= claim.last) return claim.face.get();
    }
    return nullptr;
}

const Face& FontChain::face_for(char32_t codepoint) const {
    if (const Face* claimed = claimant(codepoint)) return *claimed;
    for (const auto& face : faces_) {
        if (face->covers(codepoint)) return *face;
    }
    if (system_ != nullptr) {
        if (const Face* face = system_->face_for(codepoint)) return *face;
    }
    if (last_resort_ != nullptr && last_resort_->covers(codepoint)) return *last_resort_;
    return primary();
}

} // namespace ftk::text
