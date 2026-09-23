#include "ui/kit/asset_kind.hpp"

#include "ui/kit/icons.hpp"

#include <array>

namespace fjell::ui {

namespace {

struct Entry {
    std::string_view extension;
    AssetKind kind;
};

using C = theme::Category;

constexpr AssetKind MESH{"mesh", icon::mesh, C::Rendering};
constexpr AssetKind SHADER{"shader", icon::shader, C::Rendering};
constexpr AssetKind TEXTURE{"texture", icon::texture, C::Rendering};
constexpr AssetKind HDRI{"HDRI", icon::texture, C::Environment};
constexpr AssetKind BLEND{"blend space", icon::blend_space, C::Animation};
constexpr AssetKind AUDIO{"audio clip", icon::audio, C::Audio};
constexpr AssetKind UI_LAYOUT{"UI layout", icon::ui_layout, C::Ui};
constexpr AssetKind FONT{"font", icon::font, C::Ui};
constexpr AssetKind INPUT{"input", icon::input, C::Logic};

constexpr std::array ENTRIES{
    Entry{".fjmesh", MESH}, Entry{".glb", MESH}, Entry{".gltf", MESH}, Entry{".fbx", MESH},
    Entry{".fjskel", {"skeleton", icon::skeleton, C::Animation}},
    Entry{".fjmat", {"material", icon::material, C::Rendering}},
    Entry{".fjsl", SHADER},
    Entry{".png", TEXTURE}, Entry{".jpg", TEXTURE}, Entry{".jpeg", TEXTURE},
    Entry{".hdr", HDRI}, Entry{".exr", HDRI},
    Entry{".fjanim", {"animation", icon::animation, C::Animation}},
    Entry{".fjanimset", {"animation set", icon::animation, C::Animation}},
    Entry{".fjblend1D", BLEND}, Entry{".fjblend2D", BLEND},
    Entry{".fjaudio", AUDIO}, Entry{".ogg", AUDIO}, Entry{".wav", AUDIO}, Entry{".mp3", AUDIO},
    Entry{".fjvfx", {"effect", icon::vfx, C::Vfx}},
    Entry{".fjui", UI_LAYOUT}, Entry{".fjss", {"style sheet", icon::ui_layout, C::Ui}},
    Entry{".fjwidget", {"widget", icon::ui_layout, C::Ui}},
    Entry{".fjfont", FONT}, Entry{".ttf", FONT}, Entry{".otf", FONT},
    Entry{".fjsurface", {"surface", icon::surface, C::Physics}},
    Entry{".fjday", {"day profile", icon::day_profile, C::Environment}},
    Entry{".fjweather", {"weather", icon::weather, C::Environment}},
    Entry{".fjclimate", {"climate", icon::climate, C::Environment}},
    Entry{".fjwater", {"water", icon::water, C::Environment}},
    Entry{".fjterrain", {"terrain", icon::surface, C::Environment}},
    Entry{".fjell", {"scene", icon::scene, C::Structure}},
    Entry{".fjp", {"preset", icon::preset, C::Structure}},
    Entry{".fjinput", INPUT}, Entry{".fjaction", INPUT},
};

constexpr AssetKind UNKNOWN{"file", icon::file, C::Structure};

} // namespace

const AssetKind& asset_kind(std::string_view extension) {
    for (const auto& e : ENTRIES) {
        if (e.extension == extension) return e.kind;
    }
    return UNKNOWN;
}

} // namespace fjell::ui
