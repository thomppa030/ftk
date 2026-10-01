#include "ui/asset_kinds.hpp"

#include "ftk/ui/kit/asset_kind.hpp"
#include "ui/categories.hpp"
#include "ui/icons.hpp"

#include <array>
#include <string_view>

namespace fjell {

namespace {

using ftk::ui::AssetKind;
using C = Category;

struct Entry {
    std::string_view extension;
    AssetKind kind;
};

constexpr AssetKind MESH{"mesh", icon::mesh, category_hue(C::Rendering)};
constexpr AssetKind SHADER{"shader", icon::shader, category_hue(C::Rendering)};
constexpr AssetKind BLEND{"blend space", icon::blend_space, category_hue(C::Animation)};
constexpr AssetKind AUDIO{"audio clip", icon::audio, category_hue(C::Audio)};
constexpr AssetKind UI_LAYOUT{"UI layout", icon::ui_layout, category_hue(C::Ui)};
constexpr AssetKind FONT{"font", icon::font, category_hue(C::Ui)};
constexpr AssetKind INPUT{"input", icon::input, category_hue(C::Logic)};

// The engine's file types. Images, fonts and audio files the kit knows
// already.
constexpr std::array ENTRIES{
    Entry{".fjmesh", MESH}, Entry{".glb", MESH}, Entry{".gltf", MESH}, Entry{".fbx", MESH},
    Entry{".fjskel", {"skeleton", icon::skeleton, category_hue(C::Animation)}},
    Entry{".fjmat", {"material", icon::material, category_hue(C::Rendering)}},
    Entry{".fjlut", {"colour grade", icon::colour, category_hue(C::Rendering)}},
    Entry{".cube", {"colour grade table", icon::colour, category_hue(C::Rendering)}},
    Entry{".fjsl", SHADER},
    Entry{".fjanim", {"animation", icon::animation, category_hue(C::Animation)}},
    Entry{".fjanimset", {"animation set", icon::animation, category_hue(C::Animation)}},
    Entry{".fjblend1D", BLEND}, Entry{".fjblend2D", BLEND},
    Entry{".fjaudio", AUDIO},
    Entry{".fjvfx", {"effect", icon::vfx, category_hue(C::Vfx)}},
    Entry{".fjui", UI_LAYOUT}, Entry{".fjss", {"style sheet", icon::ui_layout, category_hue(C::Ui)}},
    Entry{".fjwidget", {"widget", icon::ui_layout, category_hue(C::Ui)}},
    Entry{".fjfont", FONT},
    Entry{".fjsurface", {"surface", icon::surface, category_hue(C::Physics)}},
    Entry{".fjday", {"day profile", icon::day_profile, category_hue(C::Environment)}},
    Entry{".fjweather", {"weather", icon::weather, category_hue(C::Environment)}},
    Entry{".fjclimate", {"climate", icon::climate, category_hue(C::Environment)}},
    Entry{".fjwater", {"water", icon::water, category_hue(C::Environment)}},
    Entry{".fjterrain", {"terrain", icon::surface, category_hue(C::Environment)}},
    Entry{".fjell", {"scene", icon::scene, category_hue(C::Structure)}},
    Entry{".fjp", {"preset", icon::preset, category_hue(C::Structure)}},
    Entry{".fjinput", INPUT}, Entry{".fjaction", INPUT},
};

} // namespace

void register_asset_kinds() {
    for (const Entry& entry : ENTRIES) {
        ftk::ui::register_asset_kind(entry.extension, entry.kind);
    }
}

} // namespace fjell
