#include "ui/game_ui/ui_context.hpp"

#include "ui/game_ui/ui_pipes.hpp"

namespace fjell {

UiContext::~UiContext() = default;
UiContext::UiContext(UiContext&&) noexcept = default;
UiContext& UiContext::operator=(UiContext&&) noexcept = default;

std::optional<UiValue> UiContext::read_name(std::string_view name) const {
    for (const UiContext* scope = this; scope != nullptr; scope = scope->parent_) {
        if (auto it = scope->values_.find(name); it != scope->values_.end()) return it->second;
        if (scope->self_.where() != nullptr) {
            if (UiValue field = UiValue(scope->self_).member(name); !field.empty()) return field;
        }
    }
    return std::nullopt;
}

UiValue UiContext::lookup(std::string_view path) const {
    const auto dot = path.find('.');
    std::optional<UiValue> value = read_name(path.substr(0, dot));
    if (!value) {
        if (dot == std::string_view::npos) return {};
        const UiValue* whole = find(path); // published under a dotted name
        return whole != nullptr ? *whole : UiValue();
    }
    for (auto at = dot; at != std::string_view::npos && !value->empty();) {
        const auto next = path.find('.', at + 1);
        *value = value->member(path.substr(at + 1, next - at - 1)); // to the next dot or the end
        at = next;
    }
    return std::move(*value);
}

UiValue UiContext::evaluate(std::string_view expression) const {
    return evaluate(parse_expression(expression));
}

UiValue UiContext::evaluate(const UiExpression& expression) const {
    UiValue value = lookup(expression.path);
    if (expression.pipes.empty()) return value;
    const UiPipes* formatters = pipes();
    if (formatters == nullptr) return value;
    for (const auto& call : expression.pipes) value = formatters->apply(value, call);
    return value;
}

} // namespace fjell
