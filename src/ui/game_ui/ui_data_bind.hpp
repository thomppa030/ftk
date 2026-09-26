#pragma once

#include "ui/game_ui/ui_node.hpp"
#include "ui/game_ui/ui_pipes.hpp"
#include "ui/game_ui/ui_value.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fjell {

class UiContext;

/// What an evaluation of the bindings changed.
struct DataBindResult {
    bool text_changed{false};    // text content or a bar's fill (redraw only)
    bool layout_changed{false};  // visibility, classes, inline styles or repeats (style and layout again)
    // Nodes whose templated src or material now names something else. The
    // caller loads those; the pointers hold until the tree next changes.
    std::vector<UiNode*> asset_changes;
    // Repeats stamped by this evaluation, already bound, for whoever keeps
    // something per widget in them (a controller, a Model). The pointers hold
    // until the tree next changes.
    std::vector<UiNode*> stamped;
    [[nodiscard]] bool any() const { return text_changed || layout_changed; }
    operator bool() const { return any(); } // NOLINT — implicit for test compat
};

/// How much the last resolve() did: the bindings it evaluated, and those
/// whose value had changed and were applied. A quiet frame evaluates every
/// binding on show and applies none. For tests and profiling.
struct DataBindWork {
    uint32_t evaluated{0};
    uint32_t applied{0};
};

/// Keeps a tree's bound values current. The first resolve() of a tree
/// compiles its bindings: per node, the scope it reads and each thing a
/// value drives (a widget's props, `if=`, `class:`, `style-*`, `src`,
/// `material`, text, a bar, a `foreach`), every expression parsed once.
/// Every resolve() then evaluates the bindings on show and applies only
/// those whose values differ from the last ones applied. Values are compared
/// as read, before their pipes, which format them only when they change. A subtree hidden by
/// `if=`, or by `display: none` in the last style pass, evaluates only its
/// root's props, `if=` and `class:`. `foreach="item, i in path"` repeats an
/// element per entry of a list, each repeat with a scope of its own holding
/// the entry and its index; a list that changed length stamps or removes
/// repeats at its end, and the tree's bindings are compiled again.
class UiDataBind {
public:
    /// Evaluate the bindings on show and apply the ones that changed.
    DataBindResult resolve(UiNode& root, UiContext& context);

    /// After a style pass: evaluate what the last resolve() skipped as
    /// `display: none` and is shown now, so it appears with this frame's
    /// values. `revealed` says whether anything was; the caller styles what
    /// changed and asks again until nothing is.
    DataBindResult resolve_revealed(bool& revealed);

    /// The tree was replaced: its bindings go, and the next resolve()
    /// compiles the new one.
    void forget_tree();

    /// Called with a repeat's root just before the repeat is removed, while
    /// everything in it still exists: what was kept per widget in it (a
    /// controller) has to go first.
    void set_removing_hook(std::function<void(UiNode&)> hook) { removing_ = std::move(hook); }

    [[nodiscard]] const DataBindWork& work() const { return work_; }

    /// Resolve {key} placeholders in a template string against a context.
    static std::string resolve_template(const std::string& tmpl, const UiContext& context);
    /// The same, keeping a lone `{key}` as the value it reads rather than
    /// its text: a click's `{slot.key}` hands over the int itself.
    static UiValue resolve_value(const std::string& tmpl, const UiContext& context);

    /// Deep-clone a UiNode subtree. Copies structural data (tag, attributes,
    /// classes, text, widget state) but not runtime state (yg_node,
    /// repeat_scope, scoped_context).
    static std::unique_ptr<UiNode> clone_node(const UiNode& source, UiNode* new_parent);

private:
    /// Text with `{expression}`s in it, parsed once: literal runs and
    /// expressions, in order.
    struct Template {
        struct Part {
            std::string literal{};
            UiExpression expression{};
            bool is_expression{false};
        };
        std::vector<Part> parts{};
        /// `{x}` and nothing else: the value itself rather than its text.
        bool single{false};

        /// Appends what each expression reads in `scope` now, before its
        /// pipes: what a binding compares from frame to frame.
        void read(const UiContext& scope, std::vector<UiValue>& out) const;
        /// Runs each expression's pipes over what it read, in place.
        void format(const UiContext& scope, std::span<UiValue> values) const;
        /// The text, with `values` in place of the expressions.
        [[nodiscard]] std::string text(std::span<const UiValue> values) const;
        [[nodiscard]] std::size_t expressions() const;
    };

    enum class Kind : uint8_t { prop, visible, class_toggle, style, src, material, text, bar, repeat };

    struct Binding {
        Kind kind{Kind::text};
        std::string name{};      ///< a prop, class or style property; a repeat's entry
        std::string type{};      ///< a prop's declared type ("" keeps the value as it is)
        std::string index{};     ///< a repeat's index name; empty for none
        Template value{};        ///< what it shows; a condition's left side; a bar's value
        Template other{};        ///< a `class:` comparison's right side; a bar's max
        bool compares{false};    ///< `class:name="a==b"` rather than a truth test
        UiNode* repeated{nullptr}; ///< a repeat's template node; `value` is its list
        std::vector<UiValue> last{}; ///< what the expressions read when last applied, before their pipes
        bool applied{false};
    };

    struct NodeEntry {
        UiNode* node;
        UiContext* scope;       ///< what its bindings read
        UiContext* outer;       ///< what its props read: the scope around the widget
        uint32_t first_binding; ///< its props, `if=` and `class:` come first
        uint32_t gate_end;      ///< the rest are skipped while it is hidden
        uint32_t end_binding;
        uint32_t subtree_end;   ///< the entry after its last descendant's
    };

    static Template compile(const std::string& text);
    static Template compile_expression(std::string_view expression);

    void build(UiNode& root, UiContext& context);
    void add_node(UiNode& node, UiContext& outer);
    /// Evaluates entries [first, end), skipping hidden subtrees. False when
    /// a repeat changed the tree and the table has to be compiled again.
    bool evaluate_nodes(std::size_t first, std::size_t end, DataBindResult& result);
    /// A shown node's bindings after its gate, repeats among them.
    bool evaluate_rest(const NodeEntry& entry, DataBindResult& result);
    void evaluate(Binding& binding, const NodeEntry& entry, DataBindResult& result);
    /// Applies a binding with its values as shown: read, then formatted.
    void apply(const Binding& binding, std::span<const UiValue> shown, UiNode& node, DataBindResult& result) const;
    /// Stamps or removes repeats to match the list and gives each its entry
    /// and index. True when the tree changed.
    bool repeat(Binding& binding, const NodeEntry& entry, DataBindResult& result);
    /// Compile the table again and evaluate all of it.
    void rebuild_and_evaluate(DataBindResult& result);
    /// Each widget in a stamped repeat gets a scope of its own, as the
    /// resolver gave the template's: under the nearest scope around it, with
    /// its props' defaults in it.
    static void give_widget_scopes(UiNode& node, UiContext& around);

    std::vector<NodeEntry> nodes_;
    std::vector<Binding> bindings_;
    std::vector<std::size_t> skipped_; ///< entries skipped as display: none
    std::vector<UiValue> scratch_;
    std::vector<UiValue> shown_;
    std::function<void(UiNode&)> removing_;
    UiNode* built_root_{nullptr};
    UiContext* built_context_{nullptr};
    bool templates_marked_{false}; ///< build hid new foreach templates: a layout change
    DataBindWork work_;
};

} // namespace fjell
