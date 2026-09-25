#include "ui/game_ui/ui_context.hpp"

#include "ui/game_ui/ui_pipes.hpp"

namespace fjell {

UiContext::~UiContext() = default;
UiContext::UiContext(UiContext&&) noexcept = default;
UiContext& UiContext::operator=(UiContext&&) noexcept = default;

UiValue UiContext::lookup(std::string_view path) const {
    const auto dot = path.find('.');
    const UiValue* first = find(std::string(path.substr(0, dot)));
    if (first == nullptr) {
        if (dot == std::string_view::npos) return {};
        const UiValue* whole = find(std::string(path)); // published under a dotted name
        return whole != nullptr ? *whole : UiValue();
    }
    UiValue value = *first;
    for (auto at = dot; at != std::string_view::npos && !value.empty();) {
        const auto next = path.find('.', at + 1);
        value = value.member(path.substr(at + 1, next - at - 1)); // to the next dot or the end
        at = next;
    }
    return value;
}

UiValue UiContext::evaluate(std::string_view expression) const {
    const UiExpression expr = parse_expression(expression);
    UiValue value = lookup(expr.path);
    if (expr.pipes.empty()) return value;
    const UiPipes* formatters = pipes();
    if (formatters == nullptr) return value;
    for (const auto& call : expr.pipes) value = formatters->apply(value, call);
    return value;
}

} // namespace fjell
