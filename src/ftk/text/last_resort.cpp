#include "ftk/text/last_resort.hpp"

#include <cstddef>
#include <cstring>
#include <vector>

namespace ftk::text {

namespace {

// LastResortHE-Regular.ttf from ftk/fonts (SIL Open Font License, beside it).
constexpr char LAST_RESORT[] = {
#include "LastResortHE-Regular.ttf.inc"
};

} // namespace

std::shared_ptr<const Face> last_resort() {
    static const std::shared_ptr<const Face> face = [] {
        std::vector<std::byte> data(sizeof(LAST_RESORT));
        std::memcpy(data.data(), LAST_RESORT, sizeof(LAST_RESORT));
        auto loaded = Face::load(std::move(data), 0, "the Last Resort font");
        return loaded ? *loaded : nullptr;
    }();
    return face;
}

} // namespace ftk::text
