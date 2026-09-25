#pragma once

#include "core/delegate.hpp"
#include "ui/game_ui/ui_value.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fjell {

class UiContext;
class UiPipes;

/// The items of a list, for `foreach`. A deque, because each repeat the
/// markup stamps out keeps a pointer to its item: a deque grown or shrunk at
/// the back leaves every surviving item where it was.
using UiList = std::deque<UiContext>;

/// Hashes a name as text, so a scope is searched with a string_view and no
/// std::string is built to look one up.
struct UiNameHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view name) const noexcept {
        return std::hash<std::string_view>{}(name);
    }
};

/// Bridge between game logic and UI. Game code sets values and registers
/// action callbacks; the UI reads values via {key} and dispatches actions.
///
/// A name is read from the nearest scope that has it: its published values
/// (props, Models), then the fields of its own object (the widget's
/// controller), then its parent. dispatch() bubbles up to the parent if the
/// action is not bound here.
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

    /// The object whose fields and getters this scope reads by name, after
    /// its published values: a widget's controller. It is read in place, so
    /// it must outlive the scope or be cleared first.
    void set_self(UiObject object) { self_ = object; }
    [[nodiscard]] const UiObject& self() const { return self_; }

    /// Publish a value; `{key}` shows it and `if=`/`class:` test it. An
    /// unchanged value (the same type holding the same value) changes nothing.
    void set(const std::string& key, UiValue value) {
        auto it = values_.find(key);
        if (it != values_.end() && it->second == value) return; // no change
        values_.insert_or_assign(key, std::move(value));
        ++generation_;
    }

    [[nodiscard]] uint64_t generation() const { return generation_; }

    /// The value published under exactly this name here, or in the nearest
    /// parent that has one; null when no scope does. Paths, and a
    /// controller's fields, go through lookup().
    [[nodiscard]] const UiValue* find(std::string_view key) const {
        auto it = values_.find(key);
        if (it != values_.end()) return &it->second;
        return parent_ ? parent_->find(key) : nullptr;
    }

    /// The value a path reaches: `name`, or `name.field.field` into an
    /// object (a Model, a struct, a getter's result) or a vector. The first
    /// name is read through the scopes, each later one from the value before
    /// it. A name no scope holds reads the whole path as one name, so a value
    /// published under a dotted name is still found. No value when nothing
    /// is there.
    [[nodiscard]] UiValue lookup(std::string_view path) const;

    /// The value a path reaches as markup shows it, or "" when there is none.
    [[nodiscard]] std::string get(const std::string& path) const { return lookup(path).text(); }

    /// What a binding's braces hold, `path` or `path|pipe:arg|pipe`: the
    /// value, formatted by its pipes left to right. No value when the path
    /// reaches nothing.
    [[nodiscard]] UiValue evaluate(std::string_view expression) const;

    /// The pipes bindings format with: this scope's, or the nearest parent's.
    /// The root scope of a UI gets its UI's.
    void set_pipes(const UiPipes* pipes) { pipes_ = pipes; }
    [[nodiscard]] const UiPipes* pipes() const {
        if (pipes_ != nullptr) return pipes_;
        return parent_ != nullptr ? parent_->pipes() : nullptr;
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

    /// Replace a list's items for foreach directives. Each item becomes a
    /// context whose parent is this one. The items are moved into the slots
    /// the list already has, so a repeat pointing at an item that survives
    /// the change still points at a live context.
    void set_list(const std::string& key, std::vector<UiContext> items) {
        auto& list = lists_[key];
        list.resize(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            list[i] = std::move(items[i]);
            list[i].set_parent(this);
        }
        ++generation_;
    }

    /// Grow or shrink a list at its end. Surviving items keep their values
    /// and their place; new ones start empty, reading through to this
    /// context. Resizing to the current size changes nothing.
    void set_list_size(const std::string& key, size_t count) {
        auto& list = lists_[key];
        if (list.size() == count) return;
        while (list.size() > count) list.pop_back();
        while (list.size() < count) list.emplace_back(this);
        ++generation_;
    }

    /// Set a value on one item of a list. Ignored when the list does not
    /// exist or the index is past its end; an unchanged value changes
    /// nothing, here or on the item.
    void set_item(const std::string& key, size_t index, const std::string& field, UiValue value) {
        auto it = lists_.find(key);
        if (it == lists_.end() || index >= it->second.size()) return;
        auto& item = it->second[index];
        uint64_t before = item.generation();
        item.set(field, std::move(value));
        if (item.generation() != before) ++generation_;
    }

    /// Get a list by key, or nullptr if not found.
    [[nodiscard]] UiList* get_list(const std::string& key) {
        auto it = lists_.find(key);
        return it != lists_.end() ? &it->second : nullptr;
    }

    [[nodiscard]] const UiList* get_list(const std::string& key) const {
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
    /// A name's value in the nearest scope that holds it, as lookup() reads
    /// the first part of a path; nothing when no scope does.
    [[nodiscard]] std::optional<UiValue> read_name(std::string_view name) const;

    UiContext* parent_{nullptr};
    const UiPipes* pipes_{nullptr};
    UiObject self_{};
    uint64_t generation_{0};
    std::unordered_map<std::string, UiValue, UiNameHash, std::equal_to<>> values_;
    std::unordered_map<std::string, std::unique_ptr<Delegate<void()>>> actions_;
    std::unordered_map<std::string, std::unique_ptr<Delegate<void(const std::vector<std::string>&)>>> param_actions_;
    std::unordered_map<std::string, UiList> lists_;
};

} // namespace fjell
