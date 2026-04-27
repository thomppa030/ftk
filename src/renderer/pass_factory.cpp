#include "renderer/pass_factory.hpp"

#include <algorithm>

namespace fjell {

std::unordered_map<std::string, PassFactory::Maker>& PassFactory::registry() {
    // Function-local static avoids static-init-order issues across TUs.
    static std::unordered_map<std::string, Maker> map;
    return map;
}

void PassFactory::register_class(std::string class_name, Maker maker) {
    registry()[std::move(class_name)] = std::move(maker);
}

std::unique_ptr<RenderPass> PassFactory::make(const std::string& class_name,
                                              const PassParams& params) {
    auto it = registry().find(class_name);
    if (it == registry().end()) { return nullptr; }
    return it->second(params);
}

bool PassFactory::has(const std::string& class_name) {
    return registry().contains(class_name);
}

std::vector<std::string> PassFactory::registered_classes() {
    std::vector<std::string> names;
    names.reserve(registry().size());
    for (const auto& [name, _] : registry()) { names.push_back(name); }
    std::ranges::sort(names);
    return names;
}

} // namespace fjell
