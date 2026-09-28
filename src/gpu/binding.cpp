#include "gpu/binding.hpp"

#include <algorithm>

namespace fjell::gpu {

namespace {

const char* kind_name(BindingKind kind) {
    switch (kind) {
        case BindingKind::uniform_buffer:         return "a uniform buffer";
        case BindingKind::storage_buffer:         return "a storage buffer";
        case BindingKind::sampled_texture:        return "a sampled texture";
        case BindingKind::texture:                return "a texture";
        case BindingKind::sampler:                return "a sampler";
        case BindingKind::storage_texture:        return "a storage texture";
        case BindingKind::acceleration_structure: return "an acceleration structure";
    }
    return "a resource";
}

bool fits(const ShaderBinding& declared, const SharedLayoutDesc& shared) {
    for (const auto& offered : shared.bindings) {
        if (offered.binding != declared.binding || offered.kind != declared.kind) continue;
        const bool offered_array = offered.count != 1;
        // An array sized at run time fits any array; a fixed one fits an
        // array at least as long; a single binding fits only a single one,
        // so a pass's own texture never reads as the bindless table.
        if (declared.count == 0) return offered_array;
        if (declared.count == 1) return !offered_array;
        return offered.count == 0 || declared.count <= offered.count;
    }
    return false;
}

} // namespace

Result<PlacedSet> place(const ShaderLayout& layout, std::span<const BindEntry> entries) {
    if (entries.empty()) return make_error("Nothing to bind");

    // Each entry's binding, checked on its own.
    SmallVector<const ShaderBinding*, INLINE_SET_ENTRIES> declared;
    for (const BindEntry& entry : entries) {
        const ShaderBinding* binding = layout.find(entry.name);
        if (binding == nullptr) {
            return make_error("The shaders declare no binding '" + std::string(entry.name) + "'");
        }
        if (!declared.empty() && binding->set != declared[0]->set) {
            return make_error("'" + std::string(entry.name) + "' is in set " +
                              std::to_string(binding->set) + ", the others in set " +
                              std::to_string(declared[0]->set) + ": one bind fills one set");
        }
        if (entry.resource.kind != binding->kind) {
            return make_error("'" + binding->name + "' is " + kind_name(binding->kind) + ", given " +
                              kind_name(entry.resource.kind));
        }
        if (binding->count == 0) {
            return make_error("'" + binding->name +
                              "' is an array sized at run time, which only a shared group holds");
        }
        if (entry.element >= binding->count) {
            return make_error("'" + binding->name + "' has " + std::to_string(binding->count) +
                              " elements, given element " + std::to_string(entry.element));
        }
        declared.push_back(binding);
    }

    // The set's bindings in their order, each element filled exactly once:
    // the entries come out in binding order with nothing to sort. A set with
    // a table sized at run time is a shared group's, whatever is given here.
    PlacedSet placed;
    placed.set = declared[0]->set;
    for (const ShaderBinding& binding : layout.bindings) {
        if (binding.set != placed.set) continue;
        if (binding.count == 0) {
            return make_error("Set " + std::to_string(placed.set) + " holds '" + binding.name +
                              "', an array sized at run time, which only a shared group holds");
        }
        for (uint32_t element = 0; element < binding.count; ++element) {
            const BindEntry* filled = nullptr;
            for (size_t i = 0; i < entries.size(); ++i) {
                if (declared[i] != &binding || entries[i].element != element) continue;
                if (filled != nullptr) {
                    return make_error("Binding " + std::to_string(binding.binding) + " element " +
                                      std::to_string(element) + " of set " +
                                      std::to_string(placed.set) + " is given twice");
                }
                filled = &entries[i];
            }
            if (filled == nullptr) {
                std::string what = "'" + binding.name + "'";
                if (binding.count > 1) what += " element " + std::to_string(element);
                return make_error("Set " + std::to_string(placed.set) + " is missing " + what);
            }
            placed.entries.push_back({binding.binding, element, filled->resource});
        }
    }
    return placed;
}

Result<std::vector<uint32_t>> place_shared(const ShaderLayout& layout,
                                           std::span<const SharedLayoutDesc> shared) {
    std::vector<uint32_t> sets_declared;
    for (const auto& binding : layout.bindings) {
        if (std::ranges::find(sets_declared, binding.set) == sets_declared.end()) {
            sets_declared.push_back(binding.set);
        }
    }

    std::vector<uint32_t> placed;
    for (const SharedLayoutDesc& group : shared) {
        std::vector<uint32_t> candidates;
        for (uint32_t set : sets_declared) {
            const bool all_fit = std::ranges::all_of(layout.bindings, [&](const ShaderBinding& b) {
                return b.set != set || fits(b, group);
            });
            if (all_fit) candidates.push_back(set);
        }
        uint32_t chosen = 0;
        if (std::ranges::find(candidates, group.usual_set) != candidates.end()) {
            chosen = group.usual_set;
        } else if (candidates.size() == 1) {
            chosen = candidates.front();
        } else if (candidates.empty()) {
            return make_error("The shaders declare no set that fits the shared '" + group.name + "'");
        } else {
            std::string sets;
            for (uint32_t set : candidates) sets += (sets.empty() ? "" : ", ") + std::to_string(set);
            return make_error("The shared '" + group.name + "' fits sets " + sets +
                              ", none of them its usual set " + std::to_string(group.usual_set));
        }
        for (size_t i = 0; i < placed.size(); ++i) {
            if (placed[i] == chosen) {
                return make_error("The shared '" + shared[i].name + "' and '" + group.name +
                                  "' both fit set " + std::to_string(chosen) + " and only one can");
            }
        }
        placed.push_back(chosen);
    }
    return placed;
}

} // namespace fjell::gpu
