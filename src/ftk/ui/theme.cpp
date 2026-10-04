#include "ftk/ui/theme.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <utility>

namespace ftk::theme {

namespace {

// The approved dark palette, palettes/dark.json, built in.
constexpr char DARK[] = {
#include "dark.json.inc"
};

struct TokenName {
    Token token;
    std::string_view name;
};

// Every token, by the name a palette gives it.
constexpr TokenName TOKEN_NAMES[] = {
    {Token::surface_sunken, "surface_sunken"},
    {Token::surface_base, "surface_base"},
    {Token::surface_raised, "surface_raised"},
    {Token::surface_hover, "surface_hover"},
    {Token::surface_active, "surface_active"},
    {Token::surface_highest, "surface_highest"},
    {Token::surface_inset, "surface_inset"},
    {Token::surface_between, "surface_between"},
    {Token::surface_grabbed, "surface_grabbed"},
    {Token::border, "border"},
    {Token::backdrop, "backdrop"},
    {Token::shadow, "shadow"},
    {Token::text, "text"},
    {Token::text_secondary, "text_secondary"},
    {Token::text_disabled, "text_disabled"},
    {Token::text_on_accent, "text_on_accent"},
    {Token::accent, "accent"},
    {Token::accent_hov, "accent_hov"},
    {Token::accent_dim, "accent_dim"},
    {Token::accent_bright, "accent_bright"},
    {Token::selection, "selection"},
    {Token::selection_secondary, "selection_secondary"},
    {Token::drop_fits, "drop_fits"},
    {Token::toggle_on, "toggle_on"},
    {Token::success, "success"},
    {Token::warning, "warning"},
    {Token::error, "error"},
    {Token::danger, "danger"},
    {Token::danger_hov, "danger_hov"},
    {Token::text_on_danger, "text_on_danger"},
    {Token::axis_x, "axis_x"},
    {Token::axis_y, "axis_y"},
    {Token::axis_z, "axis_z"},
    {Token::grid_minor, "grid_minor"},
    {Token::grid_major, "grid_major"},
    {Token::grid_zero, "grid_zero"},
    {Token::layout_margin, "layout_margin"},
    {Token::layout_padding, "layout_padding"},
    {Token::layout_box, "layout_box"},
    {Token::sdf_inside_near, "sdf_inside_near"},
    {Token::sdf_inside_far, "sdf_inside_far"},
    {Token::sdf_outside_near, "sdf_outside_near"},
    {Token::sdf_outside_far, "sdf_outside_far"},
    {Token::code_keyword, "code_keyword"},
    {Token::code_type, "code_type"},
    {Token::code_function, "code_function"},
    {Token::code_string, "code_string"},
    {Token::code_number, "code_number"},
    {Token::code_comment, "code_comment"},
    {Token::code_meta, "code_meta"},
    {Token::code_punctuation, "code_punctuation"},
    {Token::code_member, "code_member"},
    {Token::code_builtin, "code_builtin"},
    {Token::code_current_line, "code_current_line"},
    {Token::code_indent_guide, "code_indent_guide"},
    {Token::code_indent_guide_active, "code_indent_guide_active"},
    {Token::code_occurrence, "code_occurrence"},
    {Token::code_bracket, "code_bracket"},
    {Token::code_error_line, "code_error_line"},
    {Token::code_warning_line, "code_warning_line"},
};
static_assert(std::size(TOKEN_NAMES) == static_cast<std::size_t>(Token::count));

// The hues, by name, in Hue's order.
constexpr std::string_view HUE_NAMES[] = {
    "blue", "ochre", "teal", "cyan", "coral", "pink", "orange", "periwinkle", "lilac", "slate", "grey",
};
static_assert(std::size(HUE_NAMES) == HUE_COUNT);

constexpr std::string_view HUES_KEY = "hues";
constexpr std::string_view FOLDERS_KEY = "folder_colours";

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Result<ui::Color> unreadable(std::string_view name, const nlohmann::json& value) {
    return make_error("palette: " + std::string(name) + " is " + value.dump() +
                      ", not \"#RRGGBB\", \"#RRGGBB NN%\" or [r, g, b, a]");
}

/// "#RRGGBB", "#RRGGBB 40%" or [r, g, b, a]: the colour `name` is given.
Result<ui::Color> parse_colour(std::string_view name, const nlohmann::json& value) {
    if (value.is_array()) {
        if (value.size() != 4) return unreadable(name, value);
        ui::Color colour{};
        for (int i = 0; i < 4; ++i) {
            if (!value[static_cast<std::size_t>(i)].is_number()) return unreadable(name, value);
            colour[i] = value[static_cast<std::size_t>(i)].get<float>();
        }
        return colour;
    }
    if (!value.is_string()) return unreadable(name, value);
    const std::string_view text = value.get_ref<const std::string&>();
    if (text.size() < 7 || text[0] != '#') return unreadable(name, value);
    float channels[3]{};
    for (int i = 0; i < 3; ++i) {
        const int high = hex_digit(text[static_cast<std::size_t>(1 + i * 2)]);
        const int low = hex_digit(text[static_cast<std::size_t>(2 + i * 2)]);
        if (high < 0 || low < 0) return unreadable(name, value);
        channels[i] = static_cast<float>(high * 16 + low) / 255.0f;
    }
    float alpha = 1.0f;
    if (std::string_view rest = text.substr(7); !rest.empty()) {
        if (rest.size() < 3 || rest.front() != ' ' || rest.back() != '%') return unreadable(name, value);
        rest = rest.substr(1, rest.size() - 2);
        float percent = 0.0f;
        const auto [end, failed] = std::from_chars(rest.data(), rest.data() + rest.size(), percent);
        if (failed != std::errc{} || end != rest.data() + rest.size() || percent < 0.0f || percent > 100.0f) {
            return unreadable(name, value);
        }
        alpha = percent / 100.0f;
    }
    return ui::Color{channels[0], channels[1], channels[2], alpha};
}

bool is_token_name(std::string_view name) {
    for (const TokenName& token : TOKEN_NAMES) {
        if (token.name == name) return true;
    }
    return false;
}

Palette& in_use() {
    static Palette palette = [] {
        Result<Palette> dark = parse_palette({DARK, sizeof(DARK)});
        if (!dark) {
            std::fprintf(stderr, "ftk: the built-in palette does not read: %s\n", dark.error().c_str());
            std::abort();
        }
        return std::move(*dark);
    }();
    return palette;
}

} // namespace

std::string_view name(Token token) {
    for (const TokenName& named : TOKEN_NAMES) {
        if (named.token == token) return named.name;
    }
    return {};
}

std::string_view name(Hue hue) {
    return HUE_NAMES[static_cast<std::size_t>(hue)];
}

Result<Palette> parse_palette(std::string_view json) {
    const nlohmann::json root = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) return make_error("palette: not a JSON object");

    for (const auto& [key, value] : root.items()) {
        if (key != HUES_KEY && key != FOLDERS_KEY && !is_token_name(key)) {
            return make_error("palette: " + key + " is not a token");
        }
    }

    Palette palette;
    for (const TokenName& token : TOKEN_NAMES) {
        const auto it = root.find(std::string(token.name));
        if (it == root.end()) return make_error("palette: no colour for " + std::string(token.name));
        FTK_TRY(palette.tokens[static_cast<std::size_t>(token.token)], parse_colour(token.name, *it));
    }

    const auto hues = root.find(std::string(HUES_KEY));
    if (hues == root.end() || !hues->is_object()) return make_error("palette: no hues");
    for (const auto& [key, value] : hues->items()) {
        if (std::find(std::begin(HUE_NAMES), std::end(HUE_NAMES), key) == std::end(HUE_NAMES)) {
            return make_error("palette: " + key + " is not a hue");
        }
    }
    for (std::size_t i = 0; i < HUE_COUNT; ++i) {
        const auto it = hues->find(std::string(HUE_NAMES[i]));
        if (it == hues->end()) return make_error("palette: no hue " + std::string(HUE_NAMES[i]));
        FTK_TRY(palette.hues[i], parse_colour(HUE_NAMES[i], *it));
    }

    const auto folders = root.find(std::string(FOLDERS_KEY));
    if (folders == root.end() || !folders->is_array()) return make_error("palette: no folder_colours");
    for (const nlohmann::json& folder : *folders) {
        const auto name = folder.find("name");
        const auto colour = folder.find("colour");
        if (!folder.is_object() || name == folder.end() || !name->is_string() || colour == folder.end()) {
            return make_error("palette: a folder colour is " + folder.dump() + ", not {\"name\", \"colour\"}");
        }
        NamedColour named{.name = name->get<std::string>()};
        FTK_TRY(named.colour, parse_colour(named.name, *colour));
        palette.folder_colours.push_back(std::move(named));
    }
    return palette;
}

const Palette& palette() {
    return in_use();
}

void use_palette(Palette palette) {
    in_use() = std::move(palette);
}

} // namespace ftk::theme
