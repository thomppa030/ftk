#pragma once

#include "core/delegate.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace fjell {

/// Bridge between game logic and UI. Game code sets values and registers
/// action callbacks; the UI reads values via {key} and dispatches actions.
class UiContext {
public:
    void set(const std::string& key, const std::string& value) {
        values_[key] = value;
    }

    void set_float(const std::string& key, float value) {
        values_[key] = std::to_string(value);
    }

    void set_int(const std::string& key, int value) {
        values_[key] = std::to_string(value);
    }

    [[nodiscard]] std::string get(const std::string& key) const {
        auto it = values_.find(key);
        return it != values_.end() ? it->second : "";
    }

    template <typename F>
        requires std::invocable<F>
    [[nodiscard]] Connection bind_action(const std::string& name, F&& callback) {
        auto& delegate = actions_[name];
        if (!delegate) {
            delegate = std::make_unique<Delegate<void()>>();
        }
        return delegate->bind(std::forward<F>(callback));
    }

    void dispatch(const std::string& action) {
        auto it = actions_.find(action);
        if (it != actions_.end() && it->second) {
            it->second->broadcast();
        }
    }

    void clear() {
        values_.clear();
        actions_.clear();
    }

private:
    std::unordered_map<std::string, std::string> values_;
    std::unordered_map<std::string, std::unique_ptr<Delegate<void()>>> actions_;
};

} // namespace fjell
