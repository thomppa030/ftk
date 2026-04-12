#pragma once

#include "core/delegate.hpp"

#include <charconv>
#include <memory>
#include <optional>
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
        typed_floats_[key] = value;
        typed_ints_.erase(key);
        typed_bools_.erase(key);
        char buf[32];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
        set(key, std::string(buf, ptr));
    }

    void set_int(const std::string& key, int value) {
        typed_ints_[key] = value;
        typed_floats_.erase(key);
        typed_bools_.erase(key);
        set(key, std::to_string(value));
    }

    void set_bool(const std::string& key, bool value) {
        typed_bools_[key] = value;
        typed_floats_.erase(key);
        typed_ints_.erase(key);
        set(key, value ? "1" : "0");
    }

    [[nodiscard]] uint64_t generation() const { return generation_; }

    [[nodiscard]] std::string get(const std::string& key) const {
        auto it = values_.find(key);
        if (it != values_.end()) return it->second;
        if (parent_) return parent_->get(key);
        return "";
    }

    [[nodiscard]] std::optional<float> get_float(const std::string& key) const {
        auto it = typed_floats_.find(key);
        if (it != typed_floats_.end()) return it->second;
        // Fall back to int (controllers often use [[fj::bind]] int for values)
        auto iit = typed_ints_.find(key);
        if (iit != typed_ints_.end()) return static_cast<float>(iit->second);
        if (parent_) return parent_->get_float(key);
        return std::nullopt;
    }

    [[nodiscard]] std::optional<int> get_int(const std::string& key) const {
        auto it = typed_ints_.find(key);
        if (it != typed_ints_.end()) return it->second;
        if (parent_) return parent_->get_int(key);
        return std::nullopt;
    }

    [[nodiscard]] std::optional<bool> get_bool(const std::string& key) const {
        auto it = typed_bools_.find(key);
        if (it != typed_bools_.end()) return it->second;
        if (parent_) return parent_->get_bool(key);
        return std::nullopt;
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
        typed_floats_.clear();
        typed_ints_.clear();
        typed_bools_.clear();
        actions_.clear();
        param_actions_.clear();
        lists_.clear();
    }

private:
    UiContext* parent_{nullptr};
    uint64_t generation_{0};
    std::unordered_map<std::string, std::string> values_;
    std::unordered_map<std::string, float> typed_floats_;
    std::unordered_map<std::string, int> typed_ints_;
    std::unordered_map<std::string, bool> typed_bools_;
    std::unordered_map<std::string, std::unique_ptr<Delegate<void()>>> actions_;
    std::unordered_map<std::string, std::unique_ptr<Delegate<void(const std::vector<std::string>&)>>> param_actions_;
    std::unordered_map<std::string, std::vector<UiContext>> lists_;
};

} // namespace fjell
