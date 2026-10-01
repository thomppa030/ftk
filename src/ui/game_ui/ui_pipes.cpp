#include "ui/game_ui/ui_pipes.hpp"
#include "scripting/script_call.hpp"

#include "ftk/base/log.hpp"
#include "scripting/reflected_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace fjell {

namespace {

std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t");
    return std::string(s.substr(first, last - first + 1));
}

/// Splits at `separator`, but not inside single quotes.
std::vector<std::string_view> split_outside_quotes(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    bool quoted = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\'') quoted = !quoted;
        else if (text[i] == separator && !quoted) {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(text.substr(start));
    return parts;
}

std::string argument(std::string_view raw) {
    auto text = trim(raw);
    if (text.size() >= 2 && text.front() == '\'' && text.back() == '\'') return text.substr(1, text.size() - 2);
    return text;
}

std::string fixed(float value, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(value));
    return buf;
}

std::optional<int> int_argument(const UiPipeCall& call, std::size_t index, int fallback) {
    if (call.args.size() <= index) return fallback;
    try {
        std::size_t used = 0;
        const int n = std::stoi(call.args[index], &used);
        if (used != call.args[index].size() || n < 0 || n > 9) return std::nullopt;
        return n;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

UiExpression parse_expression(std::string_view text) {
    UiExpression expr;
    auto parts = split_outside_quotes(text, '|');
    expr.path = trim(parts.front());
    for (std::size_t i = 1; i < parts.size(); ++i) {
        auto pieces = split_outside_quotes(parts[i], ':');
        UiPipeCall call;
        call.name = trim(pieces.front());
        for (std::size_t a = 1; a < pieces.size(); ++a) call.args.push_back(argument(pieces[a]));
        expr.pipes.push_back(std::move(call));
    }
    return expr;
}

std::optional<UiForeach> parse_foreach(std::string_view text) {
    auto is_name = [](std::string_view name) {
        return !name.empty() && std::ranges::all_of(name, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        });
    };
    const auto in = text.find(" in ");
    if (in == std::string_view::npos) return std::nullopt;
    const std::string_view names = text.substr(0, in);
    const auto comma = names.find(',');
    UiForeach out{.item = trim(names.substr(0, comma)),
                  .index = comma != std::string_view::npos ? trim(names.substr(comma + 1)) : std::string(),
                  .list = trim(text.substr(in + 4))};
    if (!is_name(out.item) || (comma != std::string_view::npos && !is_name(out.index)) || out.list.empty()) {
        return std::nullopt;
    }
    return out;
}

bool UiPipes::is_engine_pipe(std::string_view name) {
    return name == "round" || name == "fixed" || name == "seconds" || name == "times" || name == "percent";
}

void UiPipes::set_game_pipes(const ReflectedTypes* types) { game_ = types; }

void UiPipes::report(const std::string& message) const {
    if (reported_.insert(message).second) FJELL_CORE_WARN("Game UI: {}", message);
}

UiValue UiPipes::apply(const UiValue& value, const UiPipeCall& call) const {
    const auto& name = call.name;
    if (is_engine_pipe(name)) {
        if (game_ != nullptr && game_->find_pipe(name) != nullptr) {
            report("the game's pipe '" + name + "' is hidden by the engine's; rename it");
        }
        if (name == "times") return UiValue("×" + value.text());
        const auto number = value.number();
        if (!number) return value; // formats numbers; text that is not one stays
        // Halves away from zero, as a player reads 2.5; printf alone would
        // round them to even. Adding zero turns -0 into 0.
        if (name == "round") return UiValue(fixed(std::round(*number) + 0.0f, 0));
        if (name == "fixed" || name == "percent") {
            const auto decimals = int_argument(call, 0, 0);
            if (!decimals) {
                report("|" + name + ":" + call.args[0] + " takes a number of decimals from 0 to 9");
                return value;
            }
            if (name == "fixed") return UiValue(fixed(*number, *decimals));
            return UiValue(fixed(*number * 100.0f, *decimals) + "%");
        }
        // seconds: a countdown, so it shows 3, 2, 1 rather than 2, 1, 0.
        const int total = std::max(0, static_cast<int>(std::ceil(*number)));
        if (total < 60) return UiValue(std::to_string(total));
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
        return UiValue(std::string(buf));
    }
    UiValue out;
    if (call_game_pipe(value, call, out)) return out;
    return value;
}

bool UiPipes::call_game_pipe(const UiValue& value, const UiPipeCall& call, UiValue& out) const {
    // Without a game module there is nobody to ask, and the name may be one
    // of its pipes: the value shows unformatted, and that is all.
    if (game_ == nullptr) return false;
    const OwnedPipeDesc* pipe = game_->find_pipe(call.name);
    if (pipe == nullptr) {
        report("no pipe named '" + call.name + "'");
        return false;
    }
    if (call.args.size() != pipe->arg_types.size()) {
        report("|" + call.name + " takes " + std::to_string(pipe->arg_types.size()) + " argument(s), given " +
               std::to_string(call.args.size()));
        return false;
    }

    // The value and each argument as the type the function declares.
    int input_int = 0;
    float input_float = 0.0f;
    bool input_bool = false;
    std::string input_text;
    const void* input = nullptr;
    switch (pipe->input) {
        case FJELL_FIELD_INT:
            input_int = static_cast<int>(std::trunc(value.number().value_or(0.0f)));
            input = &input_int;
            break;
        case FJELL_FIELD_FLOAT:
            input_float = value.number().value_or(0.0f);
            input = &input_float;
            break;
        case FJELL_FIELD_BOOL:
            input_bool = value.truthy();
            input = &input_bool;
            break;
        case FJELL_FIELD_STD_STRING:
            input_text = value.text();
            input = &input_text;
            break;
        default:
            report("|" + call.name + " formats a kind of value markup cannot hand it");
            return false;
    }
    std::vector<int> ints(call.args.size());
    std::vector<float> floats(call.args.size());
    std::vector<const void*> args(call.args.size());
    for (std::size_t i = 0; i < call.args.size(); ++i) {
        const auto& text = call.args[i];
        try {
            switch (pipe->arg_types[i]) {
                case FJELL_FIELD_INT: ints[i] = std::stoi(text); args[i] = &ints[i]; break;
                case FJELL_FIELD_FLOAT: floats[i] = std::stof(text); args[i] = &floats[i]; break;
                default: args[i] = &text; break;
            }
        } catch (...) {
            report("|" + call.name + ": argument " + std::to_string(i + 1) + " '" + text + "' is not a number");
            return false;
        }
    }
    std::string text;
    const ScriptCall script{.type_name = call.name, .hook = "pipe"};
    if (!call_script(script, [&] { pipe->call(input, args.data(), &text); })) return false;
    out = UiValue(std::move(text));
    return true;
}

} // namespace fjell
