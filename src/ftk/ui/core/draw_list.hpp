#pragma once

// What the UI draws, recorded as shapes rather than triangles (decision 16
// of the UI core plan): each is one instance the renderer draws as a quad,
// whose fragment shader works out the shape's distance at every pixel. Edges
// are exact at any scale, and outlines, round corners and soft edges cost
// nothing extra. The calls are those of the kit's ui::DrawList, which
// forwards here once the kit draws with the core.
//
//     DrawList& dl = ui.draw_list();
//     dl.rect_filled(min, max, theme::surface_sunken(), 4.0f);
//     dl.push_clip(min, max);
//     dl.text({x, y}, theme::text_secondary(), "Opening");
//     dl.pop_clip();
//
// Points are in layout units from the root's top-left; the root's scale
// turns them into pixels. What falls wholly outside the clip is dropped as
// it is recorded.

#include "ftk/ui/core/types.hpp"

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ftk::ui::core {

class TextSystem;

enum class PrimitiveKind : uint32_t {
    /// A rectangle from min to max, its corners rounded by `shape`'s radii
    /// (top-left, top-right, bottom-right, bottom-left), shading from
    /// `colour` at its left edge to `colour2` at its right. Filled, or an
    /// outline `thickness` wide inside its edge. With a texture, the picture
    /// fills it, `extra` holding its coordinates (min, max). A circle is one
    /// whose radii are half its size.
    Rect,
    /// A line from shape.xy to shape.zw, `thickness` wide, with round ends.
    Segment,
    /// Part of a circle's outline around shape.xy, shape.z in radius, from
    /// angle extra.x to extra.y in radians: 0 to the right, growing
    /// clockwise on screen. `thickness` wide.
    Arc,
    /// A filled triangle with corners shape.xy, shape.zw and extra.xy.
    /// extra.z marks the edges that lie inside a larger shape, which neither
    /// fade nor count as near: 1 for shape.xy to shape.zw, 2 for shape.zw to
    /// extra.xy, 4 for extra.xy back to shape.xy. Two triangles of one
    /// shape meet on such an edge without a seam.
    Triangle,
    /// A glyph: atlas page `texture`'s coverage at `extra` (min, max), in
    /// `colour`.
    Glyph,
};

/// One shape as the renderer reads it from a storage buffer (std430): 80
/// bytes. min and max bound the shape itself; the renderer widens the quad
/// by the antialiased edge and `softness`.
struct Primitive {
    glm::vec2 min{0.0f};
    glm::vec2 max{0.0f};
    glm::vec4 shape{0.0f};
    glm::vec4 extra{0.0f};
    /// Red, green, blue and alpha in sRGB, a byte each from the lowest:
    /// what unpackUnorm4x8 reads.
    uint32_t colour{0};
    uint32_t colour2{0};
    /// An outline's width; zero fills.
    float thickness{0.0f};
    /// How far the edge fades out, for a shadow; zero is a crisp edge.
    float softness{0.0f};
    PrimitiveKind kind{PrimitiveKind::Rect};
    /// Index into DrawData::clips.
    uint32_t clip{0};
    /// A glyph's atlas page; a picture's index into DrawData::textures,
    /// where zero is none.
    uint32_t texture{0};
    uint32_t reserved{0};
};
static_assert(sizeof(Primitive) == 80, "Primitive is uploaded as it is");

struct ClipRect {
    glm::vec2 min{0.0f};
    glm::vec2 max{0.0f};
};

/// What one root drew this frame, in the order it is drawn.
struct DrawData {
    std::vector<Primitive> primitives{};
    /// The rectangles primitives are clipped to; the first is the whole
    /// root.
    std::vector<ClipRect> clips{};
    /// The pictures shown; the first is none.
    std::vector<Texture> textures{};
};

/// `colour` as a primitive holds it.
[[nodiscard]] uint32_t pack_colour(Color colour);

class DrawList {
public:
    DrawList() = default;

    /// Starts a frame's drawing into `data`, cleared, for a root `size`
    /// big at `scale` pixels to the unit. Text is laid out by `text`.
    void reset(DrawData& data, glm::vec2 size, float scale, TextSystem* text);

    void rect_filled(glm::vec2 min, glm::vec2 max, Color colour, float rounding = 0.0f,
                     RoundCorners corners = RoundCorners::All);
    void rect(glm::vec2 min, glm::vec2 max, Color colour, float rounding = 0.0f,
              RoundCorners corners = RoundCorners::All, float thickness = 1.0f);
    /// Filled, shading from `left` at its left edge to `right` at its right.
    void rect_gradient(glm::vec2 min, glm::vec2 max, Color left, Color right);
    /// A soft shadow under a rectangle: `colour` fading out over `softness`.
    void shadow(glm::vec2 min, glm::vec2 max, Color colour, float rounding, float softness);

    void line(glm::vec2 from, glm::vec2 to, Color colour, float thickness = 1.0f);
    /// Lines through `points` in order, and back to the first when `closed`.
    void polyline(std::span<const glm::vec2> points, Color colour, float thickness = 1.0f, bool closed = false);

    void circle_filled(glm::vec2 centre, float radius, Color colour);
    void circle(glm::vec2 centre, float radius, Color colour, float thickness = 1.0f);
    /// Part of a circle's outline, from angle `from` to angle `to` in
    /// radians: 0 to the right, growing clockwise on screen.
    void arc(glm::vec2 centre, float radius, float from, float to, Color colour, float thickness = 1.0f);

    void triangle_filled(glm::vec2 a, glm::vec2 b, glm::vec2 c, Color colour);
    void triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, Color colour, float thickness = 1.0f);
    /// A convex quad, its corners in order around it.
    void quad_filled(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d, Color colour);
    /// A convex polygon, its corners in order around it: triangles fanned
    /// from the first corner, edged only where the polygon is.
    void convex_filled(std::span<const glm::vec2> points, Color colour);
    void quad(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d, Color colour, float thickness = 1.0f);

    /// `text` in the current text style, its top-left at `at`. A newline
    /// starts another line.
    void text(glm::vec2 at, Color colour, std::string_view text);
    /// The same, wrapped to lines `wrap_width` wide.
    void text_wrapped(glm::vec2 at, Color colour, std::string_view text, float wrap_width);
    /// One glyph from the atlas, `min` to `max`, its coverage at `uv_min` to
    /// `uv_max` of atlas page `page`: what text is made of.
    void glyph(glm::vec2 min, glm::vec2 max, glm::vec2 uv_min, glm::vec2 uv_max, uint32_t page, Color colour);

    /// `texture` filling `min`..`max`, its corners rounded by `rounding`.
    void image(Texture texture, glm::vec2 min, glm::vec2 max, float rounding = 0.0f,
               glm::vec2 uv_min = {0.0f, 0.0f}, glm::vec2 uv_max = {1.0f, 1.0f});

    /// What follows is drawn only within `min`..`max` and the clip already
    /// there, until the matching pop_clip().
    void push_clip(glm::vec2 min, glm::vec2 max);
    void pop_clip();
    /// The clip rectangle what is drawn now falls within.
    [[nodiscard]] glm::vec2 clip_min() const;
    [[nodiscard]] glm::vec2 clip_max() const;

    /// What follows goes into `target`, cleared unless `keep`, until
    /// pop_target(): a panel's own shapes, put in order over the others when
    /// the root ends. Clips start again from the root's.
    void push_target(std::vector<Primitive>& target, bool keep = false);
    void pop_target();
    /// How much the current target holds, and taking it back to that: what
    /// was drawn for a container that isn't to be seen yet.
    [[nodiscard]] std::size_t mark() const { return target_->size(); }
    void truncate(std::size_t mark);
    /// Puts what was drawn since `split` beneath what was drawn from `mark`
    /// to `split`: a box drawn once what it holds is, and its size known.
    void put_under(std::size_t mark, std::size_t split);

    /// Pixels to the unit.
    [[nodiscard]] float scale() const { return scale_; }
    [[nodiscard]] const DrawData& data() const { return *data_; }

private:
    struct Target {
        std::vector<Primitive>* previous{nullptr};
        std::size_t clip_depth{0};
    };

    // Records `p` under the current clip, unless its bounds fall outside it.
    void add(Primitive p);
    void segment(glm::vec2 from, glm::vec2 to, uint32_t colour, float thickness);
    // `inner` as PrimitiveKind::Triangle's extra.z holds it.
    void triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, uint32_t colour, uint32_t inner);

    DrawData* data_{nullptr};
    std::vector<Primitive>* target_{nullptr};
    TextSystem* text_{nullptr};
    float scale_{1.0f};
    std::vector<uint32_t> clip_stack_;
    std::vector<Target> targets_;
};

} // namespace ftk::ui::core
