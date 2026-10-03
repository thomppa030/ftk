#include "ftk/text/font_chain.hpp"

#include <algorithm>

namespace ftk::text {

FontChain::FontChain(std::vector<std::shared_ptr<const Face>> faces, std::shared_ptr<SystemFallback> system,
                     std::shared_ptr<const Face> last_resort)
    : faces_{std::move(faces)}, system_{std::move(system)}, last_resort_{std::move(last_resort)} {}

FontChain& FontChain::claim(std::shared_ptr<const Face> face, char32_t first, char32_t last, ClaimSeat seat) {
    claims_.push_back({.face = std::move(face), .first = first, .last = last, .seat = seat});
    return *this;
}

namespace {

// A face's ascent to descent, as a share of its em.
float height_of(const Face& face) {
    const Metrics m = face.metrics(1.0f);
    return m.ascent + m.descent;
}

} // namespace

FontChain& FontChain::size_by(SizeBasis basis) {
    basis_ = basis;
    // Asked of the face once: em() is asked for every string drawn.
    primary_height_ = faces_.empty() ? 0.0f : height_of(primary());
    return *this;
}

float FontChain::em(float size) const {
    if (basis_ == SizeBasis::Em || faces_.empty()) return size;
    return primary_height_ > 0.0f ? size / primary_height_ : size;
}

float FontChain::face_em(const Face& face, float em) const {
    if (basis_ == SizeBasis::Em || faces_.empty() || &face == &primary()) return em;
    const bool claimed = std::ranges::any_of(claims_, [&](const Claim& claim) { return claim.face.get() == &face; });
    if (!claimed) return em;
    const float own = height_of(face);
    return own > 0.0f ? em * primary_height_ / own : em;
}

float FontChain::drop(const Face& face, float size) const {
    for (const Claim& claim : claims_) {
        if (claim.face.get() != &face || claim.seat != ClaimSeat::Centred) continue;
        // A face's middle, above its baseline: halfway between its ascent
        // and its descent, at the size it is drawn at.
        const auto middle = [](const Face& of, float at) {
            const Metrics m = of.metrics(at);
            return (m.ascent - m.descent) * 0.5f;
        };
        return middle(face, face_em(face, size)) - middle(primary(), size);
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
