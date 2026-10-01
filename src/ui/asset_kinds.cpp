#include "ui/asset_kinds.hpp"

#include "ftk/ui/kit/asset_kind.hpp"
#include "ui/icons.hpp"

#include <array>
#include <string_view>

namespace fjell {

namespace {

using ftk::ui::AssetKind;
using C = ftk::theme::Category;

struct Entry {
    std::string_view extension;
    AssetKind kind;
};

constexpr AssetKind MESH{"mesh", icon::mesh, C::Rendering};
constexpr AssetKind SHADER{"shader", icon::shader, C::Rendering};
constexpr AssetKind BLEND{"blend space", icon::blend_space, C::Animation};
constexpr AssetKind AUDIO{"audio clip", icon::audio, C::Audio};
constexpr AssetKind UI_LAYOUT{"UI layout", icon::ui_layout, C::Ui};
constexpr AssetKind FONT{"font", icon::font, C::Ui};
constexpr AssetKind INPUT{"input", icon::input, C::Logic};

// The engine's file types. Images, fonts and audio files the kit knows
// already.
constexpr std::array ENTRIES{
    Entry{".fjmesh", MESH}, Entry{".glb", MESH}, Entry{".gltf", MESH}, Entry{".fbx", MESH},
    Entry{".fjskel", {"skeleton", icon::skeleton, C::Animation}},
    Entry{".fjmat", {"material", icon::material, C::Rendering}},
    Entry{".fjlut", {"colour grade", icon::colour, C::Rendering}},
    Entry{".cube", {"colour grade table", icon::colour, C::Rendering}},
    Entry{".fjsl", SHADER},
    Entry{".fjanim", {"animation", icon::animation, C::Animation}},
    Entry{".fjanimset", {"animation set", icon::animation, C::Animation}},
    Entry{".fjblend1D", BLEND}, Entry{".fjblend2D", BLEND},
    Entry{".fjaudio", AUDIO},
    Entry{".fjvfx", {"effect", icon::vfx, C::Vfx}},
    Entry{".fjui", UI_LAYOUT}, Entry{".fjss", {"style sheet", icon::ui_layout, C::Ui}},
    Entry{".fjwidget", {"widget", icon::ui_layout, C::Ui}},
    Entry{".fjfont", FONT},
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

} // namespace

void register_asset_kinds() {
    for (const Entry& entry : ENTRIES) {
        ftk::ui::register_asset_kind(entry.extension, entry.kind);
    }
}

} // namespace fjell
