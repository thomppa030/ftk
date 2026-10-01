#include "ftk/gpu/command_list.hpp"

#include <format>
#include <string>

namespace ftk::gpu {

Result<uint32_t> push_size(const ShaderLayout& layout, size_t given) {
    if (layout.push_size == 0) return make_error("the shaders take no push data");
    if (given < layout.push_size) {
        return make_error("the shaders read " + std::to_string(layout.push_size) +
                          " bytes of push data, given " + std::to_string(given));
    }
    return layout.push_size;
}

uint32_t declared_sets(const ShaderLayout& layout) {
    uint32_t sets = 0;
    for (const auto& binding : layout.bindings) sets |= 1u << binding.set;
    return sets;
}

Result<> barrier_accesses(AccessSet before, AccessSet after, bool (*applies)(Access) noexcept,
                          std::string_view resource) {
    for (const AccessSet side : {before, after}) {
        if (const auto wrong = first_misapplied(side, applies)) {
            return make_error(std::format("{} is not an access {} has", access_name(*wrong), resource));
        }
    }
    return {};
}

} // namespace ftk::gpu
