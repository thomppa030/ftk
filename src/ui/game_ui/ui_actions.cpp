#include "ui/game_ui/ui_actions.hpp"

#include "scripting/reflected_types.hpp"

#include <glm/vec2.hpp>

#include <cmath>
#include <optional>

namespace fjell {

namespace {

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return std::string(text.substr(first, text.find_last_not_of(" \t") - first + 1));
}

const char* takes(FjellUiActionParam param) {
    switch (param) {
        case FJELL_UI_ACTION_NONE: return "nothing";
        case FJELL_UI_ACTION_INT: return "a whole number";
        case FJELL_UI_ACTION_FLOAT: return "a number";
        case FJELL_UI_ACTION_VEC2: return "a vec2, or two numbers";
    }
    return "nothing";
}

/// How many arguments a call to the action may have.
bool count_fits(FjellUiActionParam param, std::size_t count) {
    switch (param) {
        case FJELL_UI_ACTION_NONE: return count == 0;
        case FJELL_UI_ACTION_INT:
        case FJELL_UI_ACTION_FLOAT: return count == 1;
        case FJELL_UI_ACTION_VEC2: return count == 1 || count == 2;
    }
    return false;
}

std::string wrong_count(const OwnedUiActionDesc& action, std::size_t count) {
    return action.name + " takes " + takes(action.param) + ", given " + std::to_string(count) + " argument" +
           (count == 1 ? "" : "s");
}

} // namespace

UiActionCall parse_action_call(std::string_view text) {
    UiActionCall call;
    const auto open = text.find('(');
    const auto close = text.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
        call.name = trim(text);
        return call;
    }
    call.name = trim(text.substr(0, open));
    const std::string_view inside = text.substr(open + 1, close - open - 1);
    if (trim(inside).empty()) return call;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= inside.size(); ++i) {
        if (i == inside.size() || inside[i] == ',') {
            call.args.push_back(trim(inside.substr(start, i - start)));
            start = i + 1;
        }
    }
    return call;
}

std::string call_action(const OwnedUiActionDesc& action, void* self, std::span<const UiValue> args) {
    if (action.invoke == nullptr) return action.name + " cannot be called: rebuild the game module";
    if (!count_fits(action.param, args.size())) return wrong_count(action, args.size());

    auto not_a_number = [&](const UiValue& value) {
        return action.name + " takes " + takes(action.param) + "; '" + value.text() + "' is not one";
    };
    switch (action.param) {
        case FJELL_UI_ACTION_NONE:
            action.invoke(self, nullptr);
            return {};
        case FJELL_UI_ACTION_INT: {
            const auto n = args[0].number();
            if (!n) return not_a_number(args[0]);
            const int whole = static_cast<int>(std::trunc(*n));
            action.invoke(self, &whole);
            return {};
        }
        case FJELL_UI_ACTION_FLOAT: {
            const auto n = args[0].number();
            if (!n) return not_a_number(args[0]);
            const float value = *n;
            action.invoke(self, &value);
            return {};
        }
        case FJELL_UI_ACTION_VEC2: {
            glm::vec2 value{0.0f};
            if (args.size() == 2) {
                const auto x = args[0].number();
                const auto y = args[1].number();
                if (!x) return not_a_number(args[0]);
                if (!y) return not_a_number(args[1]);
                value = {*x, *y};
            } else if (const UiVector* v = args[0].vector(); v != nullptr && v->length >= 2) {
                value = {v->value.x, v->value.y};
            } else {
                return not_a_number(args[0]);
            }
            action.invoke(self, &value);
            return {};
        }
    }
    return {};
}

std::string check_action_call(const OwnedUiActionDesc& action, const UiActionCall& call) {
    if (!count_fits(action.param, call.args.size())) return wrong_count(action, call.args.size());
    if (action.param == FJELL_UI_ACTION_NONE) return {};
    // A lone literal for a vec2 cannot be one; two must both be numbers.
    if (action.param == FJELL_UI_ACTION_VEC2 && call.args.size() == 1 &&
        call.args[0].find('{') == std::string::npos) {
        return action.name + " takes " + takes(action.param) + "; '" + call.args[0] + "' is neither";
    }
    for (const auto& arg : call.args) {
        if (arg.find('{') != std::string::npos) continue; // judged when clicked
        if (!UiValue(arg).number()) {
            return action.name + " takes " + takes(action.param) + "; '" + arg + "' is not a number";
        }
    }
    return {};
}

} // namespace fjell
