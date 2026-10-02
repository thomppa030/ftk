#include "ftk/text/font_chain.hpp"

namespace ftk::text {

FontChain::FontChain(std::vector<std::shared_ptr<const Face>> faces) : faces_{std::move(faces)} {}

const Face& FontChain::face_for(char32_t codepoint) const {
    for (const auto& face : faces_) {
        if (face->covers(codepoint)) return *face;
    }
    return primary();
}

} // namespace ftk::text
