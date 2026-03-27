#pragma once

#include "core/delegate.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace fjell {

/// Bridge between game logic and UI. Game code sets values and registers
/// action callbacks; the UI reads values via {key} and dispatches actions.
///
/// Supports scoped parent chain: get() walks up to parent if key not found
/// locally, dispatch() bubbles up to parent if action not found locally.
class UiContext {
public:
    UiContext() = default;
    explicit UiContext(UiContext* parent) : parent_{parent} {}

    void set_parent(UiContext* parent) { parent_ = parent; }
    [[nodiscard]] UiContext* parent() const { return parent_; }

    void set(const std::string& key, const std::string& value) {
        values_[key] = value;
    }

    void set_float(const std::string& key, float value) {
        values_[key] = std::to_string(value);
    }

    void set_int(const std::string& key, int value) {
        values_[key] = std::to_string(value);
    }

    void set_bool(const std::string& key, bool value) {
        values_[key] = value ? "1" : "0";
    }

    [[nodiscard]] std::string get(const std::string& key) const {
        auto it = values_.find(key);
        if (it != values_.end()) return it->second;
        if (parent_) return parent_->get(key);
        return "";
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
            return;
        }
        if (parent_) parent_->dispatch(action);
    }

    /// Set a list of item contexts for foreach directives.
    /// Each item is a UiContext whose parent is this context.
    void set_list(const std::string& key, std::vector<UiContext> items) {
        for (auto& item : items) {
            item.set_parent(this);
        }
        lists_[key] = std::move(items);
    }

    /// Get a list by key, or nullptr if not found.
    [[nodiscard]] std::vector<UiContext>* get_list(const std::string& key) {
        auto it = lists_.find(key);
        return it != lists_.end() ? &it->second : nullptr;
    }

    [[nodiscard]] const std::vector<UiContext>* get_list(const std::string& key) const {
        auto it = lists_.find(key);
        return it != lists_.end() ? &it->second : nullptr;
    }

    void clear() {
        values_.clear();
        actions_.clear();
        lists_.clear();
    }

private:
    UiContext* parent_{nullptr};
    std::unordered_map<std::string, std::string> values_;
    std::unordered_map<std::string, std::unique_ptr<Delegate<void()>>> actions_;
    std::unordered_map<std::string, std::vector<UiContext>> lists_;
};

} // namespace fjell
