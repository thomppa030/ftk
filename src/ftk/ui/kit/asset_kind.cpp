#include "ftk/ui/kit/asset_kind.hpp"

#include "ftk/ui/kit/icons.hpp"

#include <functional>
#include <map>
#include <string>

namespace ftk::ui {

namespace {

using C = theme::Category;

constexpr AssetKind TEXTURE{"texture", icon::texture, C::Rendering};
constexpr AssetKind HDRI{"HDRI", icon::texture, C::Environment};
constexpr AssetKind FONT{"font", icon::font, C::Ui};
constexpr AssetKind AUDIO{"audio clip", icon::audio, C::Audio};
constexpr AssetKind UNKNOWN{"file", icon::file, C::Structure};

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
