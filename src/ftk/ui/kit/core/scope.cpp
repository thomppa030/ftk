#include "ftk/ui/kit/scope.hpp"

#include "ftk/ui/kit/core/kit.hpp"

namespace ftk::ui {

using namespace corekit;

Disabled::Disabled(bool disabled) {
    ui().push_disabled(disabled);
}

Disabled::~Disabled() {
    ui().pop_disabled();
}

IdScope::IdScope(std::string_view id) {
    ui().ids().push(id);
}

IdScope::IdScope(const char* id) {
    ui().ids().push(id);
}

IdScope::IdScope(int id) {
    ui().ids().push(id);
}

IdScope::IdScope(const void* id) {
    ui().ids().push(id);
}

IdScope::~IdScope() {
    ui().ids().pop();
}

Id id(std::string_view label) {
    return ui().id(label);
}

} // namespace ftk::ui
