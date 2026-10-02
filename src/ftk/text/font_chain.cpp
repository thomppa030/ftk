#include "ftk/text/font_chain.hpp"

namespace ftk::text {

FontChain::FontChain(std::vector<std::shared_ptr<const Face>> faces, std::shared_ptr<SystemFallback> system)
    : faces_{std::move(faces)}, system_{std::move(system)} {}

FontChain& FontChain::claim(std::shared_ptr<const Face> face, char32_t first, char32_t last) {
    claims_.push_back({.face = std::move(face), .first = first, .last = last});
    return *this;
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
    return primary();
}

} // namespace ftk::text
