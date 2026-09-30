#include "ui/sdf_debug_panel.hpp"

#include "renderer/pass/scene_sdf_pass.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/kit/canvas.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/overlay.hpp"
#include "ui/kit/panel.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"
#include "ui/kit/viewport_toolbar.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <string>

namespace fjell {

namespace {

inline constexpr ui::Choice<int> AXES[] = {{0, "X"}, {1, "Y"}, {2, "Z"}};

// The world axes the slice runs across and down, for each axis it cuts
// (as sdf_slice.comp lays them out).
struct Face {
    int across;
    int down;
};
constexpr Face FACES[] = {{1, 2}, {0, 2}, {0, 1}};
constexpr const char* AXIS_NAMES[] = {"X", "Y", "Z"};

constexpr float PILL_SLIDER_WIDTH = 140.0f;

} // namespace

void SdfDebugPanel::draw(SceneSdfPass& sdf) {
    if (!open_) return;
    ImGui::SetNextWindowSize({460.0f, 600.0f}, ImGuiCond_FirstUseEver);
    auto panel = ui::Panel(ui::icon::debug, "SDF Debug", &open_);
    if (!panel) return;

    const glm::vec3 lo = sdf.volume_min();
    const glm::vec3 hi = sdf.volume_max();
    const Face face = FACES[axis_];

    // Below the slice: the legend, then the volume's heading and rows.
    const float below = ImGui::GetFrameHeightWithSpacing() * 5.0f + theme::GAP_L * 2.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size{std::max(avail.x, 1.0f), std::max(avail.y - below, 120.0f)};
    const ImVec2 max{origin.x + size.x, origin.y + size.y};

    ImGui::SetNextItemAllowOverlap();  // the pill over the slice takes the mouse there
    ImGui::InvisibleButton("##slice", size);
    const bool hovered = ImGui::IsItemHovered();

    // The slice in the shape of the volume's face, centred.
    const float face_w = std::max(hi[face.across] - lo[face.across], 1e-3f);
    const float face_h = std::max(hi[face.down] - lo[face.down], 1e-3f);
    const float scale = std::min(size.x / face_w, size.y / face_h);
    const ImVec2 image_size{std::floor(face_w * scale), std::floor(face_h * scale)};
    const ImVec2 a{std::floor(origin.x + (size.x - image_size.x) * 0.5f),
                   std::floor(origin.y + (size.y - image_size.y) * 0.5f)};
    const ImVec2 b{a.x + image_size.x, a.y + image_size.y};

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, max, ImGui::GetColorU32(theme::surface_sunken()));
    if (const ImTextureID slice = imgui_texture(sdf.slice_texture(), sdf.slice_sampler())) {
        dl->AddImage(slice, a, b);
    }

    // Which texel is under the mouse, for the pass to read back.
    std::optional<glm::uvec2> texel;
    const ImVec2 mouse = ImGui::GetMousePos();
    if (hovered && mouse.x >= a.x && mouse.y >= a.y && mouse.x < b.x && mouse.y < b.y) {
        const float n = static_cast<float>(SceneSdfPass::SLICE_SIZE);
        texel = glm::uvec2{static_cast<uint32_t>((mouse.x - a.x) / image_size.x * n),
                           static_cast<uint32_t>((mouse.y - a.y) / image_size.y * n)};
    }
    sdf.request_slice(static_cast<uint32_t>(axis_), slice_, texel);
    if (texel) {
        if (const auto distance = sdf.hovered_distance()) {
            const std::string text = std::format("{:+.2f} m", *distance);
            ui::canvas_readout(dl, mouse, text.c_str(), origin, max);
        }
    }

    // Which way the slice runs, bottom left.
    const std::string directions = std::format("{} \xe2\x86\x92   {} \xe2\x86\x93", AXIS_NAMES[face.across],
                                               AXIS_NAMES[face.down]);
    const ui::OverlayPiece line[] = {{directions}};
    ImGui::PushFont(theme::mono_font(), 0.0f);
    ui::corner_note(dl, {origin.x + ui::OVERLAY_INSET, max.y - ui::OVERLAY_INSET}, ui::Corner::BottomLeft,
                    {.line = line, .ink = theme::text_secondary()});
    ImGui::PopFont();

    // The axis it cuts and where, in metres along it.
    if (auto pill = ui::ViewportPill("##cut", origin, max, ui::PillPlace::TopLeft)) {
        (void)ui::segmented("##axis", axis_, AXES);
        ImGui::SameLine();
        const float axis_lo = lo[axis_];
        const float axis_hi = hi[axis_];
        float at = axis_lo + slice_ * (axis_hi - axis_lo);
        ImGui::SetNextItemWidth(PILL_SLIDER_WIDTH);
        if (ui::slider("##at", at, axis_lo, axis_hi, ui::Unit::Metres).changed && axis_hi > axis_lo) {
            slice_ = std::clamp((at - axis_lo) / (axis_hi - axis_lo), 0.0f, 1.0f);
        }
    }

    ImGui::SetCursorScreenPos({origin.x, max.y + theme::GAP_M});
    draw_legend(sdf.max_distance());

    ui::subheading("Volume");
    if (auto table = ui::PropertyTable("##volume")) {
        glm::vec3 volume_min = lo;
        glm::vec3 volume_max = hi;
        bool moved = false;
        ui::row("Volume min", [&] {
            moved |= ui::vec3("##min", volume_min, {.speed = 0.5f, .unit = ui::Unit::Metres}).changed;
        });
        ui::row("Volume max", [&] {
            moved |= ui::vec3("##max", volume_max, {.speed = 0.5f, .unit = ui::Unit::Metres}).changed;
        });
        if (moved) sdf.set_bounds(volume_min, volume_max);
        ui::row("Resolution", [&] {
            const glm::vec3 voxel = (hi - lo) / static_cast<float>(sdf.resolution());
            const float largest = std::max({voxel.x, voxel.y, voxel.z});
            const float smallest = std::min({voxel.x, voxel.y, voxel.z});
            const std::string text =
                largest - smallest < 1e-3f
                    ? std::format("{} voxels a side, {:.2f} m each", sdf.resolution(), largest)
                    : std::format("{} voxels a side, {:.2f} \xc3\x97 {:.2f} \xc3\x97 {:.2f} m", sdf.resolution(),
                                  voxel.x, voxel.y, voxel.z);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text.c_str());
        });
    }
}

void SdfDebugPanel::draw_legend(float max_distance) {
    // Inside to the left, outside to the right, the surface a white line
    // between: the slice shader's ramp.
    constexpr float BAR_HEIGHT = 8.0f;
    ImGui::PushFont(theme::mono_font(), 0.0f);
    const std::string left = std::format("\xe2\x88\x92{:.0f} m", max_distance);
    const std::string right = std::format("+{:.0f} m", max_distance);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float line_h = ImGui::GetFrameHeight();
    const float left_w = ImGui::CalcTextSize(left.c_str()).x;
    const float right_w = ImGui::CalcTextSize(right.c_str()).x;
    const float text_y = at.y + (line_h - ImGui::GetFontSize()) * 0.5f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 ink = ImGui::GetColorU32(theme::text_secondary());
    dl->AddText({at.x, text_y}, ink, left.c_str());
    dl->AddText({at.x + width - right_w, text_y}, ink, right.c_str());

    const float bar_x0 = std::floor(at.x + left_w + theme::GAP_M);
    const float bar_x1 = std::floor(at.x + width - right_w - theme::GAP_M);
    const float mid = std::floor((bar_x0 + bar_x1) * 0.5f);
    const float y0 = std::floor(at.y + (line_h - BAR_HEIGHT) * 0.5f);
    const float y1 = y0 + BAR_HEIGHT;
    const ImU32 in_far = ImGui::GetColorU32(theme::sdf_inside_far());
    const ImU32 in_near = ImGui::GetColorU32(theme::sdf_inside_near());
    const ImU32 out_near = ImGui::GetColorU32(theme::sdf_outside_near());
    const ImU32 out_far = ImGui::GetColorU32(theme::sdf_outside_far());
    dl->AddRectFilledMultiColor({bar_x0, y0}, {mid, y1}, in_far, in_near, in_near, in_far);
    dl->AddRectFilledMultiColor({mid, y0}, {bar_x1, y1}, out_near, out_far, out_far, out_near);
    dl->AddRectFilled({mid - 1.0f, y0 - 2.0f}, {mid + 1.0f, y1 + 2.0f}, ImGui::GetColorU32(theme::text()));
    ImGui::PopFont();
    ImGui::Dummy({width, line_h});
}

} // namespace fjell
