#include "ftk/ui/kit/asset_kind.hpp"

#include "ftk/ui/kit/icons.hpp"

#include <functional>
#include <map>
#include <string>

namespace ftk::ui {

namespace {

using H = theme::Hue;

constexpr AssetKind TEXTURE{"texture", icon::texture, H::blue};
constexpr AssetKind HDRI{"HDRI", icon::texture, H::cyan};
constexpr AssetKind FONT{"font", icon::font, H::lilac};
constexpr AssetKind AUDIO{"audio clip", icon::audio, H::orange};
constexpr AssetKind UNKNOWN{"file", icon::file, H::grey};

// Every kind by extension: the general file types, then what programs
// registered. A map, so what asset_kind() hands out stays where it is when
// more are added.
std::map<std::string, AssetKind, std::less<>>& kinds() {
    static std::map<std::string, AssetKind, std::less<>> all{
        {".png", TEXTURE}, {".jpg", TEXTURE}, {".jpeg", TEXTURE},
        {".hdr", HDRI}, {".exr", HDRI},
        {".ttf", FONT}, {".otf", FONT},
        {".ogg", AUDIO}, {".wav", AUDIO}, {".mp3", AUDIO},
    };
    return all;
}

} // namespace

const AssetKind& asset_kind(std::string_view extension) {
    const auto& all = kinds();
    const auto found = all.find(extension);
    return found != all.end() ? found->second : UNKNOWN;
}

void register_asset_kind(std::string_view extension, const AssetKind& kind) {
    kinds().insert_or_assign(std::string(extension), kind);
}

} // namespace ftk::ui
