#include "ui/game_ui/ui_value.hpp"

#include <charconv>
#include <cmath>
#include <string>

namespace fjell {

namespace {

std::optional<float> read_float(const std::string& text) {
    try {
        return std::stof(text);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int> read_int(const std::string& text) {
    try {
        return std::stoi(text);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

UiValue UiValue::parse(std::string_view text, std::string_view type) {
    const std::string s(text);
    if (type == "float") {
        if (auto f = read_float(s)) return UiValue(*f);
    } else if (type == "int") {
        if (auto i = read_int(s)) return UiValue(*i);
    } else if (type == "bool") {
        return UiValue(s != "0" && s != "false");
    }
    return UiValue(s);
}

UiValue UiValue::as(std::string_view type) const {
    if (type == "float") {
        if (auto f = number()) return UiValue(*f);
    } else if (type == "int") {
        if (auto f = number()) return UiValue(static_cast<int>(std::trunc(*f)));
    } else if (type == "bool") {
        return UiValue(truthy());
    } else if (type == "string") {
        return UiValue(text());
    }
    return *this;
}

std::string UiValue::text() const {
    if (const auto* b = std::get_if<bool>(&value_)) return *b ? "1" : "0";
    if (const auto* i = std::get_if<int>(&value_)) return std::to_string(*i);
    if (const auto* f = std::get_if<float>(&value_)) {
        char buf[32];
        auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), *f);
        return ec == std::errc{} ? std::string(buf, end) : std::string("0");
    }
    if (const auto* s = std::get_if<std::string>(&value_)) return *s;
    return {};
}

std::optional<float> UiValue::number() const {
    if (const auto* b = std::get_if<bool>(&value_)) return *b ? 1.0f : 0.0f;
    if (const auto* i = std::get_if<int>(&value_)) return static_cast<float>(*i);
    if (const auto* f = std::get_if<float>(&value_)) return *f;
    if (const auto* s = std::get_if<std::string>(&value_)) return read_float(*s);
    return std::nullopt;
}

bool UiValue::truthy() const {
    if (const auto* b = std::get_if<bool>(&value_)) return *b;
    if (const auto* i = std::get_if<int>(&value_)) return *i != 0;
    if (const auto* f = std::get_if<float>(&value_)) return *f != 0.0f;
    if (const auto* s = std::get_if<std::string>(&value_)) return !s->empty() && *s != "0" && *s != "false";
    return false;
}

} // namespace fjell
