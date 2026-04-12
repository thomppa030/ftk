#pragma once

#include "renderer/pass/render_pass.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <unordered_map>

namespace fjell {

/// Type-erased storage for render passes. Passes are registered by name
/// and looked up by concrete type via get<T>(). Owns all pass instances.
class PassRegistry {
public:
    /// Register a pass by name. Takes ownership.
    template<typename T>
    T& add(const std::string& name, std::unique_ptr<T> pass) {
        auto* ptr = pass.get();
        passes_[name] = std::move(pass);
        return *ptr;
    }

    /// Look up a pass by name, cast to concrete type. Asserts if not found.
    template<typename T>
    [[nodiscard]] T& get(const std::string& name) {
        auto it = passes_.find(name);
        assert(it != passes_.end() && "PassRegistry::get: pass not found");
        return static_cast<T&>(*it->second);
    }

    /// Look up a pass by name, cast to concrete type. Returns nullptr if not found.
    template<typename T>
    [[nodiscard]] T* try_get(const std::string& name) {
        auto it = passes_.find(name);
        if (it == passes_.end()) return nullptr;
        return static_cast<T*>(it->second.get());
    }

    /// Destroy all passes (in unspecified order).
    void destroy_all() {
        for (auto& [name, pass] : passes_) {
            pass->destroy();
        }
        passes_.clear();
    }

    /// Check if a pass is registered.
    [[nodiscard]] bool has(const std::string& name) const {
        return passes_.contains(name);
    }

private:
    std::unordered_map<std::string, std::unique_ptr<RenderPass>> passes_;
};

} // namespace fjell
