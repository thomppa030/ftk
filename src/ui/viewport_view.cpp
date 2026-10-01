#include "ui/viewport_view.hpp"

#include "ftk/ui/kit/menu.hpp"
#include "renderer/pass/ddgi_pass.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/vulkan_context.hpp"

#include <imgui.h>

#include <iterator>
#include <span>

namespace fjell {

namespace {

struct Channel {
    DebugLightChannel value;
    const char* name;
};

struct DdgiView {
    DdgiDebugMode value;
    const char* name;
};

struct BufferView {
    ViewportSource value;
    const char* name;
};

constexpr Channel LIGHTING[] = {
    {DebugLightChannel::DirectLight, "Direct light"},
    {DebugLightChannel::IblDiffuse, "IBL diffuse"},
    {DebugLightChannel::IblSpecular, "IBL specular"},
    {DebugLightChannel::DdgiIndirect, "DDGI indirect"},
    {DebugLightChannel::SsGiIndirect, "SS-GI indirect"},
    {DebugLightChannel::Emissive, "Emissive"},
    {DebugLightChannel::SunShafts, "Sun shafts"},
};

// In pipeline order: stepping down the list finds the first stage where an
// artifact appears.
constexpr Channel AO_CHAIN[] = {
    {DebugLightChannel::DepthRecon, "Depth input"},
    {DebugLightChannel::NormalRecon, "Surface normal"},
    {DebugLightChannel::AoRaw, "Raw AO"},
    {DebugLightChannel::AoUv, "Lookup UV (red/green ramp)"},
    {DebugLightChannel::AoTexels, "Texel grid (checker per AO texel)"},
    {DebugLightChannel::AoApplied, "Applied to ambient"},
};

constexpr Channel OTHER[] = {
    {DebugLightChannel::WaterTiles, "Water: simulated tiles"},
};

constexpr DdgiView DDGI_VIEWS[] = {
    {DdgiDebugMode::GridCell, "Grid cell (hashed per cage cell)"},
    {DdgiDebugMode::ProbeStatus, "Probe status (green active, red buried, blue asleep)"},
    {DdgiDebugMode::CageWeight, "Cage weight (dark = weights collapsed)"},
    {DdgiDebugMode::NoVisibility, "No visibility test (dark = probe data)"},
    {DdgiDebugMode::ProbeDistance, "Probe lattice (bright at probes)"},
    {DdgiDebugMode::TraceReach, "Trace reach (magenta outside grid)"},
    {DdgiDebugMode::RawProbe, "Raw probe irradiance"},
    {DdgiDebugMode::ChebReject, "Chebyshev reject (red floored, green tested)"},
};

// Every source but the lit picture, which is "Lit" at the top.
constexpr BufferView BUFFERS[] = {
    {ViewportSource::depth, "Depth"},
    {ViewportSource::wireframe, "Wireframe"},
    {ViewportSource::normals, "Normals"},
    {ViewportSource::overdraw, "Overdraw"},
    {ViewportSource::uv, "UV"},
    {ViewportSource::light_heatmap, "Light heatmap"},
    {ViewportSource::light_grid, "Light grid"},
    {ViewportSource::meshlet_viz, "Meshlets"},
    {ViewportSource::ao, "Ambient occlusion"},
    {ViewportSource::gi_indirect, "GI indirect"},
    {ViewportSource::gi_irradiance, "GI irradiance"},
    {ViewportSource::motion_field, "Motion field"},
};
static_assert(std::size(BUFFERS) == VIEWPORT_SOURCE_COUNT - 1,
              "every ViewportSource but final_color needs an entry in the view menu");

DebugLightChannel channel_of(VulkanContext& ctx) {
    return static_cast<DebugLightChannel>(ctx.debug_light_channel());
}

// The DDGI views read only through the indirect channel: the surface
// composite multiplies anything else by albedo and kD, and a dark surface
// would read as "no data" when the lookup succeeded. Picking one picks both.
void set_ddgi_view(VulkanContext& ctx, DdgiDebugMode mode) {
    ctx.ddgi_pass().config().debug_mode = mode;
    ctx.set_debug_light_channel(static_cast<int>(mode == DdgiDebugMode::Off ? DebugLightChannel::Off
                                                                            : DebugLightChannel::DdgiIndirect));
}

void set_channel(VulkanContext& ctx, DebugLightChannel channel) {
    ctx.ddgi_pass().config().debug_mode = DdgiDebugMode::Off;
    ctx.set_debug_light_channel(static_cast<int>(channel));
}

void channel_entries(const char* heading, std::span<const Channel> channels, ViewportSource& source,
                     VulkanContext& ctx, bool lit_source, bool ddgi_view) {
    ftk::ui::menu_heading(heading);
    for (const Channel& channel : channels) {
        const bool current = lit_source && !ddgi_view && channel_of(ctx) == channel.value;
        if (ftk::ui::menu_item({.label = channel.name, .checked = current})) {
            source = ViewportSource::final_color;
            set_channel(ctx, channel.value);
        }
    }
}

} // namespace

bool view_is_lit(ViewportSource source, VulkanContext& ctx) {
    return source == ViewportSource::final_color && channel_of(ctx) == DebugLightChannel::Off;
}

const char* view_name(ViewportSource source, VulkanContext& ctx) {
    if (source != ViewportSource::final_color) {
        for (const BufferView& buffer : BUFFERS) {
            if (buffer.value == source) return buffer.name;
        }
    }
    const DdgiDebugMode ddgi = ctx.ddgi_pass().config().debug_mode;
    if (ddgi != DdgiDebugMode::Off) {
        for (const DdgiView& view : DDGI_VIEWS) {
            if (view.value == ddgi) return view.name;
        }
    }
    const DebugLightChannel channel = channel_of(ctx);
    for (auto group : {std::span<const Channel>(LIGHTING), std::span<const Channel>(AO_CHAIN),
                       std::span<const Channel>(OTHER)}) {
        for (const Channel& entry : group) {
            if (entry.value == channel) return entry.name;
        }
    }
    return "Lit";
}

void view_menu(ViewportSource& source, VulkanContext& ctx) {
    const bool lit_source = source == ViewportSource::final_color;
    const DdgiDebugMode ddgi = ctx.ddgi_pass().config().debug_mode;
    const bool ddgi_view = ddgi != DdgiDebugMode::Off;

    if (ftk::ui::menu_item({.label = "Lit", .checked = view_is_lit(source, ctx)})) {
        source = ViewportSource::final_color;
        set_channel(ctx, DebugLightChannel::Off);
    }

    ImGui::Separator();
    channel_entries("Lighting", LIGHTING, source, ctx, lit_source, ddgi_view);
    ImGui::Separator();
    channel_entries("AO chain, in order", AO_CHAIN, source, ctx, lit_source, ddgi_view);

    ImGui::Separator();
    ftk::ui::menu_heading("DDGI probes");
    for (const DdgiView& view : DDGI_VIEWS) {
        if (ftk::ui::menu_item({.label = view.name, .checked = lit_source && ddgi == view.value})) {
            source = ViewportSource::final_color;
            set_ddgi_view(ctx, view.value);
        }
    }

    ImGui::Separator();
    ftk::ui::menu_heading("Buffers");
    for (const BufferView& buffer : BUFFERS) {
        if (ftk::ui::menu_item({.label = buffer.name, .checked = source == buffer.value})) {
            source = buffer.value;
        }
    }

    ImGui::Separator();
    channel_entries("Other", OTHER, source, ctx, lit_source, ddgi_view);
}

} // namespace fjell
