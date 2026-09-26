#pragma once

#include "ui/game_ui/ui_value.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fjell {

class UiContext;
class UiPipes;
struct UiExpression;

/// Hashes a name as text, so a scope is searched with a string_view and no
/// std::string is built to look one up.
struct UiNameHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view name) const noexcept {
        return std::hash<std::string_view>{}(name);
    }
};

/// Bridge between game logic and UI. Game code sets values and answers
/// actions; the UI reads values via {key} and dispatches actions.
///
/// A name is read from the nearest scope that has it: its published values
/// (props, Models), then the fields of its own object (the widget's
/// controller), then its parent. dispatch() bubbles up to the parent if no
/// handler here takes the action.
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

    /// Publish a value; `{key}` shows it and `if=`/`class:` test it. The UI
    /// notices a change by comparing what it shows with what it showed.
    void set(const std::string& key, UiValue value) { values_.insert_or_assign(key, std::move(value)); }

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
    /// The same for an expression already parsed, as a binding keeps it.
    [[nodiscard]] UiValue evaluate(const UiExpression& expression) const;

    /// The pipes bindings format with: this scope's, or the nearest parent's.
    /// The root scope of a UI gets its UI's.
    void set_pipes(const UiPipes* pipes) { pipes_ = pipes; }
    [[nodiscard]] const UiPipes* pipes() const {
        if (pipes_ != nullptr) return pipes_;
        return parent_ != nullptr ? parent_->pipes() : nullptr;
    }

    /// Answers an action raised in this scope or one under it, with its
    /// arguments as values: true when it took it. A widget's controller
    /// answers the [[fj::action]]s its type declares; the UI editor's preview
    /// answers everything at its root.
    using ActionHandler = std::function<bool(std::string_view action, std::span<const UiValue> args)>;
    void set_action_handler(ActionHandler handler) { action_handler_ = std::move(handler); }

    /// Raise an action here: the nearest scope, this one or a parent, whose
    /// handler takes it answers it. False when none does.
    bool dispatch(std::string_view action, std::span<const UiValue> args = {}) const {
        for (const UiContext* scope = this; scope != nullptr; scope = scope->parent_) {
            if (scope->action_handler_ && scope->action_handler_(action, args)) return true;
        }
        return false;
    }

    void clear() {
        values_.clear();
        action_handler_ = nullptr;
    }

private:
    /// A name's value in the nearest scope that holds it, as lookup() reads
    /// the first part of a path; nothing when no scope does.
    [[nodiscard]] std::optional<UiValue> read_name(std::string_view name) const;

    UiContext* parent_{nullptr};
    const UiPipes* pipes_{nullptr};
    UiObject self_{};
    std::unordered_map<std::string, UiValue, UiNameHash, std::equal_to<>> values_;
    ActionHandler action_handler_;
};

} // namespace fjell
