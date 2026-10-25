#include "primitives_page.hpp"

#include "ftk/base/log.hpp"
#include "ftk/gpu/upload.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/theme.hpp"

#include <glm/vec2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ftk::example {

namespace {

namespace core = ui::core;
using glm::vec2;
using ui::RoundCorners;

constexpr float PI = std::numbers::pi_v<float>;

// The page's grid of cards: three across, two down.
constexpr vec2 ORIGIN{32.0f, 88.0f};
constexpr vec2 CARD{384.0f, 320.0f};
constexpr float GAP = 24.0f;

// The card at column `column`, row `row`; returns where its content begins.
vec2 card(Painter& p, int column, int row, std::string_view title) {
    const vec2 min = ORIGIN + vec2{static_cast<float>(column), static_cast<float>(row)} * (CARD + GAP);
    return p.card(min, min + CARD, title);
}

// A colour as the page's picture holds it: 8-bit sRGB values.
std::array<uint8_t, 4> bytes(ui::Color colour) {
    const auto byte = [](float c) { return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f)); };
    return {byte(colour.r), byte(colour.g), byte(colour.b), byte(colour.a)};
}

void rectangles(Painter& p, vec2 at, std::size_t& chip) {
    core::DrawList& dl = p.dl();
    core::Context& ui = p.ui();
    // Rounded by 0, 4, 10, and all the way: a pill.
    const float radii[] = {0.0f, 4.0f, 10.0f, 20.0f};
    for (int i = 0; i < 4; ++i) {
        const vec2 min = at + vec2{static_cast<float>(i) * 86.0f, 0.0f};
        dl.rect_filled(min, min + vec2{72.0f, 40.0f}, theme::accent(), radii[i]);
    }
    // Outlined, 1, 2 and 4 wide; and rounded on some corners only.
    const float widths[] = {1.0f, 2.0f, 4.0f};
    for (int i = 0; i < 3; ++i) {
        const vec2 min = at + vec2{static_cast<float>(i) * 86.0f, 60.0f};
        dl.rect(min, min + vec2{72.0f, 40.0f}, theme::text(), 8.0f, RoundCorners::All, widths[i]);
    }
    dl.rect_filled(at + vec2{258.0f, 60.0f}, at + vec2{300.0f, 100.0f}, theme::hue(theme::Hue::teal), 14.0f,
                   RoundCorners::Top);
    dl.rect_filled(at + vec2{306.0f, 60.0f}, at + vec2{344.0f, 100.0f}, theme::hue(theme::Hue::coral), 14.0f,
                   RoundCorners::Left);
    // A ramp, and chips in every hue: one picked, ringed in the accent, and
    // the one under the pointer ringed in the text colour.
    dl.rect_gradient(at + vec2{0.0f, 120.0f}, at + vec2{344.0f, 148.0f}, theme::hue(theme::Hue::blue),
                     theme::hue(theme::Hue::pink));
    const ui::Id chips = ui.id("##chips");
    for (std::size_t h = 0; h < theme::HUE_COUNT; ++h) {
        const vec2 min = at + vec2{static_cast<float>(h) * 28.5f, 168.0f};
        const vec2 max = min + vec2{24.0f, 24.0f};
        const core::Interaction in = ui.interact(core::hash_int(static_cast<int>(h), chips), {.min = min, .max = max});
        if (in.clicked) chip = h;
        if (in.hovered) ui.set_cursor_shape(ui::MouseCursor::Hand);
        dl.rect_filled(min, max, theme::hue(static_cast<theme::Hue>(h)), 6.0f);
        if (h == chip) {
            dl.rect(min - 3.0f, max + 3.0f, theme::accent(), 8.0f, RoundCorners::All, 2.0f);
        } else if (in.hovered) {
            dl.rect(min - 3.0f, max + 3.0f, theme::text_secondary(), 8.0f, RoundCorners::All, 1.0f);
        }
    }
    // Hairlines on whole and half units: crisp, and evenly soft.
    for (int i = 0; i < 8; ++i) {
        const float x = at.x + static_cast<float>(i) * 44.0f + (i % 2 == 0 ? 0.0f : 0.5f);
        dl.rect_filled({x, at.y + 214.0f}, {x + 1.0f, at.y + 250.0f}, theme::text_secondary());
    }
}

void circles(Painter& p, vec2 at) {
    core::DrawList& dl = p.dl();
    const float radii[] = {3.0f, 6.0f, 12.0f, 24.0f};
    const theme::Hue hues[] = {theme::Hue::ochre, theme::Hue::teal, theme::Hue::lilac, theme::Hue::orange};
    float x = at.x;
    for (int i = 0; i < 4; ++i) {
        dl.circle_filled({x + radii[i], at.y + 26.0f}, radii[i], theme::hue(hues[i]));
        x += 2.0f * radii[i] + 22.0f;
    }
    // Rings 1, 2 and 4 wide.
    const float rings[] = {1.0f, 2.0f, 4.0f};
    for (int i = 0; i < 3; ++i) {
        dl.circle(at + vec2{26.0f + static_cast<float>(i) * 64.0f, 100.0f}, 22.0f, theme::text(), rings[i]);
    }
    // A spinner, and progress over its track.
    const vec2 spinner = at + vec2{240.0f, 100.0f};
    dl.arc(spinner, 22.0f, -PI * 0.5f, PI, theme::accent(), 4.0f);
    const vec2 progress = at + vec2{310.0f, 100.0f};
    dl.circle(progress, 22.0f, theme::surface_highest(), 6.0f);
    dl.arc(progress, 22.0f, -PI * 0.5f, PI * 0.9f, theme::success(), 6.0f);
    // Arcs of a quarter, a half and three quarters.
    for (int i = 0; i < 3; ++i) {
        const vec2 centre = at + vec2{36.0f + static_cast<float>(i) * 100.0f, 200.0f};
        dl.arc(centre, 30.0f, 0.0f, PI * 0.5f * static_cast<float>(i + 1), theme::hue(theme::Hue::cyan), 2.0f);
    }
    p.body(at + vec2{0.0f, 246.0f}, "Every edge a pixel wide at any scale", theme::SMALL_TEXT, theme::text_secondary());
}

void lines(Painter& p, vec2 at) {
    core::DrawList& dl = p.dl();
    // A fan, a line every 7.5 degrees.
    const vec2 hub = at + vec2{0.0f, 110.0f};
    for (int i = 0; i <= 12; ++i) {
        const float angle = -PI * 0.5f + PI * 0.5f * static_cast<float>(i) / 12.0f;
        dl.line(hub, hub + 110.0f * vec2{std::cos(angle), std::sin(angle)}, theme::text(), 1.0f);
    }
    // 1, 1.5, 2, 3 and 6 wide.
    const float widths[] = {1.0f, 1.5f, 2.0f, 3.0f, 6.0f};
    for (int i = 0; i < 5; ++i) {
        const float y = at.y + 4.0f + static_cast<float>(i) * 22.0f;
        dl.line({at.x + 140.0f, y}, {at.x + 344.0f, y + 8.0f}, theme::accent_bright(), widths[i]);
    }
    // A curve as a chain of segments.
    std::vector<vec2> wave;
    for (int i = 0; i <= 64; ++i) {
        const float t = static_cast<float>(i) / 64.0f;
        wave.push_back(at + vec2{t * 344.0f, 186.0f + 26.0f * std::sin(t * PI * 4.0f)});
    }
    dl.polyline(wave, theme::hue(theme::Hue::pink), 2.5f);
    p.mono(at + vec2{0.0f, 238.0f}, "polyline(64 segments, 2.5)", theme::SMALL_TEXT);
}

void shapes_and_pictures(Painter& p, vec2 at, ui::Texture values, ui::Texture light, ui::Texture pixels) {
    core::DrawList& dl = p.dl();
    // Play, a diamond, and a triangle's outline.
    dl.triangle_filled(at + vec2{0.0f, 0.0f}, at + vec2{40.0f, 22.0f}, at + vec2{0.0f, 44.0f}, theme::success());
    dl.quad_filled(at + vec2{84.0f, 0.0f}, at + vec2{106.0f, 22.0f}, at + vec2{84.0f, 44.0f}, at + vec2{62.0f, 22.0f},
                   theme::warning());
    dl.triangle(at + vec2{150.0f, 44.0f}, at + vec2{172.0f, 2.0f}, at + vec2{194.0f, 44.0f}, theme::error(), 2.0f);
    dl.quad(at + vec2{220.0f, 4.0f}, at + vec2{300.0f, 0.0f}, at + vec2{290.0f, 44.0f}, at + vec2{230.0f, 40.0f},
            theme::text(), 1.5f);
    // The picture square as sRGB values, rounded as light, and a small one
    // round, read texel by texel.
    dl.image(values, at + vec2{0.0f, 72.0f}, at + vec2{104.0f, 176.0f});
    dl.image(light, at + vec2{120.0f, 72.0f}, at + vec2{224.0f, 176.0f}, 16.0f);
    dl.image(pixels, at + vec2{240.0f, 72.0f}, at + vec2{344.0f, 176.0f}, 52.0f);
    p.body(at + vec2{0.0f, 196.0f}, "Pictures as sRGB values and as light, and one read nearest", theme::SMALL_TEXT,
           theme::text_secondary());
}

void words(Painter& p, vec2 at, TextBox& box) {
    float y = at.y;
    const float sizes[] = {11.0f, 13.0f, 14.0f, 16.0f, 20.0f};
    for (const float size : sizes) {
        p.body({at.x, y}, "Hamburgefonstiv 0123456789", size, theme::text());
        y += size * 1.5f;
    }
    p.bold({at.x, y}, "Bold at 28", 28.0f);
    y += 42.0f;
    // Icons from lucide sit in the line; Japanese comes from the system.
    const std::string icons = std::string(ui::icon::save) + " Save   " + ui::icon::remove +
                              " Delete   \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
    p.body({at.x, y}, icons, 16.0f, theme::text());
    y += 32.0f;
    text_box(p, "##type here", {at.x, y}, 344.0f, box, "Click, then type; Ctrl C and Ctrl V copy and paste");
}

void clips_and_shadows(Painter& p, vec2 at, vec2& dot, vec2& grab) {
    core::DrawList& dl = p.dl();
    core::Context& ui = p.ui();
    // Shapes cut off at the edges of a clip; the orange circle is dragged
    // about inside it, and only what of it lies inside takes the pointer.
    const vec2 clip_min = at + vec2{20.0f, 8.0f};
    const vec2 clip_max = at + vec2{180.0f, 108.0f};
    dl.rect(clip_min - 1.0f, clip_max + 1.0f, theme::border(), 0.0f);
    dl.push_clip(clip_min, clip_max);
    dl.circle_filled(clip_min, 40.0f, theme::hue(theme::Hue::blue));
    const vec2 centre = clip_min + dot;
    const core::Interaction in = ui.interact(ui.id("##dot"), {.min = centre - 50.0f, .max = centre + 50.0f});
    if (in.hovered || in.held) ui.set_cursor_shape(ui::MouseCursor::ResizeAll);
    if (in.pressed) grab = in.position - centre;
    if (in.held) dot = in.position - grab - clip_min;
    dl.circle_filled(clip_min + dot, 50.0f, in.held ? theme::hue(theme::Hue::coral) : theme::hue(theme::Hue::orange));
    dl.rect_filled(clip_min + vec2{100.0f, -20.0f}, clip_min + vec2{200.0f, 30.0f}, theme::hue(theme::Hue::lilac), 10.0f);
    dl.line(clip_min + vec2{-20.0f, 90.0f}, clip_max + vec2{20.0f, -80.0f}, theme::text(), 3.0f);
    dl.pop_clip();
    // Shadows 4, 8 and 16 soft under raised plates.
    const float softness[] = {4.0f, 8.0f, 16.0f};
    for (int i = 0; i < 3; ++i) {
        const vec2 min = at + vec2{20.0f + static_cast<float>(i) * 110.0f, 150.0f};
        const vec2 max = min + vec2{84.0f, 64.0f};
        dl.shadow(min + vec2{0.0f, softness[i] * 0.5f}, max + vec2{0.0f, softness[i] * 0.5f}, theme::backdrop(), 8.0f,
                  softness[i]);
        dl.rect_filled(min, max, theme::surface_highest(), 8.0f);
    }
}

// Every pointer the frame knows, and the frames drawn: none while nothing
// happens, as the loop sleeps.
void pointers(Painter& p, vec2 at) {
    core::Context& ui = p.ui();
    std::string line = std::format("frame {}", ui.frame());
    for (std::size_t i = 0; i < ui.pointer_count(); ++i) {
        const core::Pointer& pointer = ui.pointer(i);
        if (!pointer.present) continue;
        const char* kind = "finger";
        if (pointer.type == core::PointerType::Mouse) kind = "mouse";
        if (pointer.type == core::PointerType::Pen) kind = "pen";
        line += std::format("    {} {:.0f}, {:.0f}", kind, pointer.now.position.x, pointer.now.position.y);
        if (pointer.type != core::PointerType::Mouse) line += std::format(" pressure {:.2f}", pointer.now.pressure);
        if (pointer.type == core::PointerType::Pen) {
            line += std::format(" tilt {:.2f}, {:.2f}{}", pointer.now.tilt.x, pointer.now.tilt.y,
                                pointer.eraser ? " eraser" : "");
        }
        if (pointer.buttons != 0) line += " down";
    }
    p.mono(at, line, theme::SMALL_TEXT);
}

} // namespace

PrimitivesPage::PrimitivesPage(AppWindow& app, Fonts fonts) : fonts_(fonts) {
    // A picture: a checker of two shades, washed with a ramp of two hues
    // from corner to corner.
    constexpr uint32_t SIZE = 128;
    std::vector<uint8_t> pixels(static_cast<std::size_t>(SIZE) * SIZE * 4);
    const ui::Color from = theme::hue(theme::Hue::teal);
    const ui::Color to = theme::hue(theme::Hue::lilac);
    for (uint32_t y = 0; y < SIZE; ++y) {
        for (uint32_t x = 0; x < SIZE; ++x) {
            const float t = static_cast<float>(x + y) / static_cast<float>(2 * SIZE - 2);
            ui::Color colour = from + (to - from) * t;
            if (((x / 16) + (y / 16)) % 2 == 0) colour = colour * 0.8f;
            colour.a = 1.0f;
            const auto texel = bytes(colour);
            std::copy(texel.begin(), texel.end(), pixels.begin() + static_cast<std::ptrdiff_t>((y * SIZE + x) * 4));
        }
    }
    const auto make = [&](gpu::Format format, uint32_t side, std::span<const uint8_t> texels) {
        auto texture = app.device().create(gpu::TextureDesc{
            .format = format,
            .width = side,
            .height = side,
            .use = gpu::TextureUse::sampled,
            .name = "example_picture",
        });
        if (!texture) {
            FTK_CORE_ERROR("ftk example app: the page's picture: {}", texture.error());
            return gpu::Owned<gpu::Texture>{};
        }
        app.device().upload().to_texture(texture->get(), {}, std::as_bytes(texels),
                                         {.after = gpu::Access::sampled_fragment});
        return std::move(*texture);
    };
    // The same bytes as sRGB values, and in an sRGB image, which samples
    // them as light.
    values_texture_ = make(gpu::Format::rgba8_unorm, SIZE, pixels);
    light_texture_ = make(gpu::Format::rgba8_srgb, SIZE, pixels);
    // Eight by eight of the two hues, to be read texel by texel.
    constexpr uint32_t SMALL = 8;
    std::vector<uint8_t> small(static_cast<std::size_t>(SMALL) * SMALL * 4);
    for (uint32_t y = 0; y < SMALL; ++y) {
        for (uint32_t x = 0; x < SMALL; ++x) {
            const auto texel = bytes((x + y) % 2 == 0 ? from : to);
            std::copy(texel.begin(), texel.end(), small.begin() + static_cast<std::ptrdiff_t>((y * SMALL + x) * 4));
        }
    }
    pixels_texture_ = make(gpu::Format::rgba8_unorm, SMALL, small);
    gpu::UiRenderer& renderer = app.ui_renderer();
    if (values_texture_) values_ = renderer.texture(values_texture_.get());
    if (light_texture_) light_ = renderer.texture(light_texture_.get(), {.linear = true});
    if (pixels_texture_) {
        pixels_ = renderer.texture(pixels_texture_.get(),
                                   {.sampler = app.device().sampler({.filter = gpu::Filter::nearest,
                                                                     .mip_filter = gpu::Filter::nearest,
                                                                     .address = gpu::Address::clamp})});
    }
}

void PrimitivesPage::draw(core::Context& ui, glm::vec2 size) {
    Painter p(ui, fonts_);
    p.bold({32.0f, 24.0f}, "Primitives", 24.0f);
    p.body({32.0f, 58.0f}, "Every shape the UI core draws, its distance worked out on the GPU", theme::SMALL_TEXT,
           theme::text_secondary());
    rectangles(p, card(p, 0, 0, "Rectangles"), chip_);
    circles(p, card(p, 1, 0, "Circles and arcs"));
    lines(p, card(p, 2, 0, "Lines and curves"));
    shapes_and_pictures(p, card(p, 0, 1, "Triangles, quads and pictures"), values_, light_, pixels_);
    words(p, card(p, 1, 1, "Text"), box_);
    clips_and_shadows(p, card(p, 2, 1, "Clips and shadows"), dot_, grab_);
    pointers(p, {32.0f, size.y - 32.0f});
}

} // namespace ftk::example
