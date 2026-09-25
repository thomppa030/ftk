#include "ui/game_ui/ui_context.hpp"

#include "ui/game_ui/ui_pipes.hpp"

namespace fjell {

UiContext::~UiContext() = default;
UiContext::UiContext(UiContext&&) noexcept = default;
UiContext& UiContext::operator=(UiContext&&) noexcept = default;

UiValue UiContext::evaluate(std::string_view expression) const {
    const UiExpression expr = parse_expression(expression);
    const UiValue* found = find(expr.path);
    UiValue value = found != nullptr ? *found : UiValue();
    if (expr.pipes.empty()) return value;
    const UiPipes* formatters = pipes();
    if (formatters == nullptr) return value;
    for (const auto& call : expr.pipes) value = formatters->apply(value, call);
    return value;
}

} // namespace fjell
