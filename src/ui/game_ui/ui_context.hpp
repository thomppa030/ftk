#pragma once

#include "core/delegate.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

/// Bridge between game logic and UI. Game code sets values and registers
/// action callbacks; the UI reads values via {key} and dispatches actions.
///
/// Supports scoped parent chain: get() walks up to parent if key not found
/// locally, dispatch() bubbles up to parent if action not found locally.
class UiContext {
public:
    UiContext() = default;
    ~UiContext();  // out-of-line to prevent MSVC eager member instantiation
    explicit UiContext(UiContext* parent) : parent_{parent} {}

    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;
    UiContext(UiContext&&) noexcept;
    UiContext& operator=(UiContext&&) noexcept;

    void set_parent(UiContext* parent) { parent_ = parent; }
    [[nodiscard]] UiContext* parent() const { return parent_; }

    void set(const std::string& key, const std::string& value) {
        auto it = values_.find(key);
        if (it != values_.end() && it->second == value) return; // no change
        values_[key] = value;
        ++generation_;
    }

    void set_float(const std::string& key, float value) {
        set(key, std::to_string(value));
    }

    void set_int(const std::string& key, int value) {
        set(key, std::to_string(value));
    }

    void set_bool(const std::string& key, bool value) {
        set(key, value ? "1" : "0");
    }

    [[nodiscard]] uint64_t generation() const { return generation_; }

    [[nodiscard]] std::string get(const std::string& key) const {
        auto it = values_.find(key);
        if (it != values_.end()) return it->second;
        if (parent_) return parent_->get(key);
        return "";
    }

    template <typename F>
        requires (std::invocable<F> && !std::invocable<F, const std::vector<std::string>&>)
    [[nodiscard]] Connection bind_action(const std::string& name, F&& callback) {
        auto& delegate = actions_[name];
        if (!delegate) {
            delegate = std::make_unique<Delegate<void()>>();
        }
        return delegate->bind(std::forward<F>(callback));
    }

    /// Bind an action that receives string arguments from the markup.
    /// e.g. on:click="select(2)" → callback receives {"2"}.
    template <typename F>
        requires std::invocable<F, const std::vector<std::string>&>
    [[nodiscard]] Connection bind_action(const std::string& name, F&& callback) {
        auto& param_delegate = param_actions_[name];
        if (!param_delegate) {
            param_delegate = std::make_unique<Delegate<void(const std::vector<std::string>&)>>();
        }
        return param_delegate->bind(std::forward<F>(callback));
    }

    void dispatch(const std::string& action) {
        auto it = actions_.find(action);
        if (it != actions_.end() && it->second) {
            it->second->broadcast();
            return;
        }
        if (parent_) parent_->dispatch(action);
    }

    void dispatch(const std::string& action, const std::vector<std::string>& args) {
        // Try parameterized action first
        auto pit = param_actions_.find(action);
        if (pit != param_actions_.end() && pit->second) {
            pit->second->broadcast(args);
            return;
        }
        // Fall back to no-arg action (ignore args)
        auto it = actions_.find(action);
        if (it != actions_.end() && it->second) {
            it->second->broadcast();
            return;
        }
        if (parent_) parent_->dispatch(action, args);
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
        param_actions_.clear();
        lists_.clear();
    }

private:
    UiContext* parent_{nullptr};
    uint64_t generation_{0};
    std::unordered_map<std::string, std::string> values_;
    std::unordered_map<std::string, std::unique_ptr<Delegate<void()>>> actions_;
    std::unordered_map<std::string, std::unique_ptr<Delegate<void(const std::vector<std::string>&)>>> param_actions_;
    std::unordered_map<std::string, std::vector<UiContext>> lists_;
};

} // namespace fjell
