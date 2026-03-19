#pragma once

#include <functional>
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

    void bind_action(const std::string& name, std::function<void()> callback) {
        actions_[name] = std::move(callback);
    }

    void dispatch(const std::string& action) {
        auto it = actions_.find(action);
        if (it != actions_.end()) {
            it->second();
        }
    }

    void clear() {
        values_.clear();
        actions_.clear();
    }

private:
    std::unordered_map<std::string, std::string> values_;
    std::unordered_map<std::string, std::function<void()>> actions_;
};

} // namespace fjell
