#include "ftk/ui/kit/code_view.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/text.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ftk::ui {

using namespace corekit;

namespace {

// Between the sunken surface's edges and the code.
constexpr glm::vec2 PADDING{12.0f, 8.0f};
constexpr int TAB_COLUMNS = 4;

// Each line of `text`, without its line break.
std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t end = text.find('\n', begin);
        if (end == std::string_view::npos) {
            if (begin < text.size()) lines.push_back(text.substr(begin));
            break;
        }
        lines.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
    return lines;
}

// Where `piece`, drawn from `x` on a line that starts at `line_x`, ends: its
// runs as measured, a tab to the next stop. Each run is handed to `run_at`
// with where it starts.
template <typename RunAt>
float advance(float x, float line_x, std::string_view piece, float space, RunAt&& run_at) {
    while (!piece.empty()) {
        const size_t tab = piece.find('\t');
        const std::string_view run = piece.substr(0, tab);
        if (!run.empty()) {
            run_at(x, run);
            x += measure(run).x;
        }
        if (tab == std::string_view::npos) break;
        const float stop = space * static_cast<float>(TAB_COLUMNS);
        x = line_x + (std::floor((x - line_x) / stop) + 1.0f) * stop;
        piece.remove_prefix(tab + 1);
    }
    return x;
}

// Line [`begin`, `end`) of `text` at `at`, each stretch in its colour: what
// `spans`, the first of them not before the line, cover of it, and the text
// between them plain.
void draw_line(std::string_view text, size_t begin, size_t end, std::span<const CodeSpan> spans, glm::vec2 at,
               glm::vec2 space) {
    const float drop = std::floor((theme::CODE_LINE - space.y) * 0.5f);
    float x = at.x;
    const auto piece = [&](size_t from, size_t to, CodeKind kind) {
        const Color colour = faded(code_colour(kind));
        x = advance(x, at.x, text.substr(from, to - from), space.x,
                    [&](float run_x, std::string_view run) { draw_text({run_x, at.y + drop}, colour, run); });
    };
    size_t drawn = begin;
    for (const CodeSpan& span : spans) {
        if (span.begin >= end) break;
        const size_t from = std::max(span.begin, begin);
        const size_t to = std::min(span.end, end);
        if (from > drawn) piece(drawn, from, CodeKind::text);
        if (to > from) piece(from, to, span.kind);
        drawn = std::max(drawn, to);
    }
    if (drawn < end) piece(drawn, end, CodeKind::text);
}

} // namespace

Color code_colour(CodeKind kind) {
    switch (kind) {
    case CodeKind::text: return theme::text();
    case CodeKind::keyword: return theme::code_keyword();
    case CodeKind::type: return theme::code_type();
    case CodeKind::function: return theme::code_function();
    case CodeKind::string: return theme::code_string();
    case CodeKind::number: return theme::code_number();
    case CodeKind::comment: return theme::code_comment();
    case CodeKind::meta: return theme::code_meta();
    case CodeKind::punctuation: return theme::code_punctuation();
    case CodeKind::member: return theme::code_member();
    case CodeKind::builtin: return theme::code_builtin();
    }
    return theme::text();
}

void code_view(const char* id, std::string_view text, std::span<const CodeSpan> spans) {
    core::Context& context = ui();
    context.begin_region(id, {core::FILL, core::FILL}, {.padding = PADDING});
    const core::Rect area = context.area_rect();
    core::DrawList& dl = context.draw_list();
    dl.rect_filled(area.min, area.max, faded(theme::surface_sunken()), theme::FRAME_ROUNDING);
    {
        FontScope mono(theme::mono_font(), theme::CODE_TEXT);
        const std::vector<std::string_view> lines = lines_of(text);
        const glm::vec2 space = measure(" ");

        // The widest line, for the room it scrolls across.
        float widest = 0.0f;
        for (const std::string_view line : lines) {
            widest = std::max(widest, advance(0.0f, 0.0f, line, space.x, [](float, std::string_view) {}));
        }
        const core::Rect block =
            context.layout().next({std::max(widest, context.layout().resolve_width(core::FILL)),
                                   theme::CODE_LINE * static_cast<float>(lines.size())});

        // Only the lines in sight are drawn.
        size_t first = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            const glm::vec2 at{block.min.x, block.min.y + theme::CODE_LINE * static_cast<float>(i)};
            if (out_of_clip({.min = at, .max = {block.max.x, at.y + theme::CODE_LINE}})) continue;
            const size_t begin = static_cast<size_t>(lines[i].data() - text.data());
            const size_t end = begin + lines[i].size();
            while (first < spans.size() && spans[first].end <= begin) ++first;
            draw_line(text, begin, end, spans.subspan(first), at, space);
        }
        place_still(block);
    }
    context.end_region();
    place_still(context.layout().last_item());
}

} // namespace ftk::ui
