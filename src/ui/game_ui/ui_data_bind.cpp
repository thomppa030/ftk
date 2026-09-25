#include "ui/game_ui/ui_data_bind.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"

#include <algorithm>
#include <optional>

namespace fjell {

namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

bool is_name(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

/// What a `foreach` says: the name each repeat gives its entry, the one it
/// gives its index (empty for none), and the path to the list.
struct Foreach {
    std::string item;
    std::string index;
    std::string list;
};

/// `item in path` or `item, i in path`; nothing when the text is neither.
std::optional<Foreach> parse_foreach(std::string_view text) {
    const auto in = text.find(" in ");
    if (in == std::string_view::npos) return std::nullopt;
    const std::string_view names = text.substr(0, in);
    const auto comma = names.find(',');
    Foreach out{.item = std::string(trim(names.substr(0, comma))),
                .index = comma != std::string_view::npos ? std::string(trim(names.substr(comma + 1))) : std::string(),
                .list = std::string(trim(text.substr(in + 4)))};
    if (!is_name(out.item) || (comma != std::string_view::npos && !is_name(out.index)) || out.list.empty()) {
        return std::nullopt;
    }
    return out;
}

} // namespace

// ── Templates ───────────────────────────────────────────────────────────

UiDataBind::Template UiDataBind::compile(const std::string& text) {
    Template out;
    std::string literal;
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t end = text[i] == '{' ? text.find('}', i + 1) : std::string::npos;
        if (end == std::string::npos) { // plain text, or a brace that never closes
            literal += text[i++];
            continue;
        }
        if (!literal.empty()) out.parts.push_back({.literal = std::move(literal)});
        literal.clear();
        out.parts.push_back({.expression = parse_expression(std::string_view(text).substr(i + 1, end - i - 1)),
                             .is_expression = true});
        i = end + 1;
    }
    if (!literal.empty()) out.parts.push_back({.literal = std::move(literal)});
    out.single = out.parts.size() == 1 && out.parts.front().is_expression;
    return out;
}

UiDataBind::Template UiDataBind::compile_expression(std::string_view expression) {
    Template out;
    out.parts.push_back({.expression = parse_expression(expression), .is_expression = true});
    out.single = true;
    return out;
}

void UiDataBind::Template::read(const UiContext& scope, std::vector<UiValue>& out) const {
    for (const auto& part : parts) {
        if (part.is_expression) out.push_back(scope.lookup(part.expression.path));
    }
}

void UiDataBind::Template::format(const UiContext& scope, std::span<UiValue> values) const {
    const UiPipes* pipes = scope.pipes();
    if (pipes == nullptr) return;
    std::size_t next = 0;
    for (const auto& part : parts) {
        if (!part.is_expression) continue;
        UiValue& value = values[next++];
        for (const auto& call : part.expression.pipes) value = pipes->apply(value, call);
    }
}

std::string UiDataBind::Template::text(std::span<const UiValue> values) const {
    std::string out;
    std::size_t next = 0;
    for (const auto& part : parts) out += part.is_expression ? values[next++].text() : part.literal;
    return out;
}

std::size_t UiDataBind::Template::expressions() const {
    return static_cast<std::size_t>(
        std::ranges::count_if(parts, [](const Part& part) { return part.is_expression; }));
}

std::string UiDataBind::resolve_template(const std::string& tmpl, const UiContext& context) {
    const Template compiled = compile(tmpl);
    std::vector<UiValue> values;
    compiled.read(context, values);
    compiled.format(context, values);
    return compiled.text(values);
}

// ── The table ───────────────────────────────────────────────────────────

void UiDataBind::forget_tree() {
    nodes_.clear();
    bindings_.clear();
    skipped_.clear();
    built_root_ = nullptr;
    built_context_ = nullptr;
}

void UiDataBind::build(UiNode& root, UiContext& context) {
    nodes_.clear();
    bindings_.clear();
    skipped_.clear();
    built_root_ = &root;
    built_context_ = &context;
    add_node(root, context);
}

void UiDataBind::add_node(UiNode& node, UiContext& outer) {
    // A repeat reads through its own scope, which holds its entry; a widget
    // reads its own scope, and its props the scope around it.
    UiContext& around = node.repeat_scope ? *node.repeat_scope : outer;
    UiContext& scope = node.scoped_context ? *node.scoped_context : around;
    const std::size_t index = nodes_.size();
    nodes_.push_back({.node = &node,
                      .scope = &scope,
                      .outer = &around,
                      .first_binding = static_cast<uint32_t>(bindings_.size()),
                      .gate_end = 0,
                      .end_binding = 0,
                      .subtree_end = 0});

    // A widget's props, read in the scope around it, as the types the
    // widget declares. A prop bound to one value (`{key}`) takes that value
    // itself; anything longer is text.
    if (node.is_widget && node.scoped_context) {
        for (const auto& [inner, outer_expression] : node.prop_map) {
            Binding binding{.kind = Kind::prop, .name = inner, .value = compile(outer_expression)};
            for (const auto& prop : node.widget_props) {
                if (prop.name == inner) binding.type = prop.type;
            }
            bindings_.push_back(std::move(binding));
        }
    }
    if (auto it = node.attributes.find("if"); it != node.attributes.end()) {
        bindings_.push_back({.kind = Kind::visible, .value = compile_expression(it->second)});
    }
    for (const auto& [attr, condition] : node.attributes) {
        if (attr.size() <= 6 || !attr.starts_with("class:")) continue;
        Binding binding{.kind = Kind::class_toggle, .name = attr.substr(6)};
        if (const auto eq = condition.find("=="); eq != std::string::npos) {
            binding.value = compile_expression(std::string_view(condition).substr(0, eq));
            binding.other = compile(condition.substr(eq + 2));
            binding.compares = true;
        } else {
            binding.value = compile_expression(condition);
        }
        bindings_.push_back(std::move(binding));
    }
    nodes_[index].gate_end = static_cast<uint32_t>(bindings_.size());

    for (const auto& [attr, text] : node.attributes) {
        if (attr.starts_with("style-")) {
            bindings_.push_back({.kind = Kind::style, .name = attr.substr(6), .value = compile(text)});
        }
    }
    // A src or material that names an asset outright is the loader's.
    for (auto [attr, kind] : {std::pair{"src", Kind::src}, std::pair{"material", Kind::material}}) {
        auto it = node.attributes.find(attr);
        if (it == node.attributes.end() || it->second.find('{') == std::string::npos) continue;
        bindings_.push_back({.kind = kind, .value = compile(it->second)});
    }
    if (node.type == UiNodeType::text && !node.text_template.empty()) {
        bindings_.push_back({.kind = Kind::text, .value = compile(node.text_template)});
    }
    if (node.tag == "bar") {
        auto value = node.attributes.find("value");
        auto max = node.attributes.find("max");
        if (value != node.attributes.end() && max != node.attributes.end()) {
            bindings_.push_back({.kind = Kind::bar, .value = compile(value->second), .other = compile(max->second)});
        }
    }
    // A child with foreach is the template its repeats are stamped from:
    // never shown, never bound itself.
    for (auto& child : node.children) {
        auto it = child->attributes.find("foreach");
        if (it == child->attributes.end()) continue;
        if (!child->is_foreach_template) {
            child->is_foreach_template = true;
            child->hidden_by_if = true;
            templates_marked_ = true;
        }
        const auto spec = parse_foreach(it->second);
        if (!spec) {
            FJELL_CORE_WARN("{}:{}: foreach=\"{}\" reads `item in list` or `item, i in list`",
                            child->source_file, child->source_line, it->second);
            continue;
        }
        bindings_.push_back({.kind = Kind::repeat,
                             .name = spec->item,
                             .index = spec->index,
                             .value = compile_expression(spec->list),
                             .repeated = child.get()});
    }
    nodes_[index].end_binding = static_cast<uint32_t>(bindings_.size());

    for (auto& child : node.children) {
        if (!child->is_foreach_template) add_node(*child, scope);
    }
    nodes_[index].subtree_end = static_cast<uint32_t>(nodes_.size());
}

// ── Evaluation ──────────────────────────────────────────────────────────

namespace {

bool hidden(const UiNode& node) {
    return node.hidden_by_if || node.style.display == UiDisplay::none;
}

} // namespace

DataBindResult UiDataBind::resolve(UiNode& root, UiContext& context) {
    FJELL_PROFILE_SCOPE_N("ui_data_bind_resolve");
    work_ = {};
    DataBindResult result;
    if (built_root_ != &root || built_context_ != &context) build(root, context);
    skipped_.clear();
    if (!evaluate_nodes(0, nodes_.size(), result)) rebuild_and_evaluate(result);
    if (templates_marked_) {
        templates_marked_ = false;
        result.layout_changed = true;
    }
    return result;
}

DataBindResult UiDataBind::resolve_revealed(bool& revealed) {
    FJELL_PROFILE_SCOPE_N("ui_data_bind_revealed");
    revealed = false;
    DataBindResult result;
    const std::vector<std::size_t> skipped = std::move(skipped_);
    skipped_.clear();
    for (const std::size_t index : skipped) {
        const NodeEntry& entry = nodes_[index];
        if (hidden(*entry.node)) {
            skipped_.push_back(index);
            continue;
        }
        revealed = true;
        if (!evaluate_rest(entry, result) || !evaluate_nodes(index + 1, entry.subtree_end, result)) {
            rebuild_and_evaluate(result);
            break;
        }
    }
    if (templates_marked_) {
        templates_marked_ = false;
        result.layout_changed = true;
    }
    return result;
}

void UiDataBind::rebuild_and_evaluate(DataBindResult& result) {
    do {
        build(*built_root_, *built_context_);
    } while (!evaluate_nodes(0, nodes_.size(), result));
}

bool UiDataBind::evaluate_nodes(std::size_t first, std::size_t end, DataBindResult& result) {
    for (std::size_t i = first; i < end;) {
        const NodeEntry& entry = nodes_[i];
        for (uint32_t b = entry.first_binding; b < entry.gate_end; ++b) evaluate(bindings_[b], entry, result);
        if (hidden(*entry.node)) {
            // `if=` was settled just now; a style can still reveal it this frame.
            if (!entry.node->hidden_by_if) skipped_.push_back(i);
            i = entry.subtree_end;
            continue;
        }
        if (!evaluate_rest(entry, result)) return false;
        ++i;
    }
    return true;
}

bool UiDataBind::evaluate_rest(const NodeEntry& entry, DataBindResult& result) {
    for (uint32_t b = entry.gate_end; b < entry.end_binding; ++b) {
        Binding& binding = bindings_[b];
        if (binding.kind != Kind::repeat) {
            evaluate(binding, entry, result);
        } else if (repeat(binding, entry, result)) {
            return false;
        }
    }
    return true;
}

void UiDataBind::evaluate(Binding& binding, const NodeEntry& entry, DataBindResult& result) {
    ++work_.evaluated;
    const UiContext& scope = binding.kind == Kind::prop ? *entry.outer : *entry.scope;
    scratch_.clear();
    binding.value.read(scope, scratch_);
    binding.other.read(scope, scratch_);
    if (binding.applied && scratch_ == binding.last) return;
    std::swap(binding.last, scratch_);
    binding.applied = true;
    ++work_.applied;

    // Pipes only format, so they run on what changed and never on a quiet
    // frame: formatting is most of what a binding would cost otherwise.
    shown_ = binding.last;
    const std::size_t split = binding.value.expressions();
    binding.value.format(scope, std::span<UiValue>(shown_).first(split));
    binding.other.format(scope, std::span<UiValue>(shown_).subspan(split));
    apply(binding, shown_, *entry.node, result);
}

void UiDataBind::apply(const Binding& binding, std::span<const UiValue> shown, UiNode& node,
                       DataBindResult& result) const {
    const std::span<const UiValue> values = shown;
    const std::span<const UiValue> first = values.first(binding.value.expressions());
    const std::span<const UiValue> second = values.subspan(first.size());
    switch (binding.kind) {
        case Kind::prop: {
            UiValue value = binding.value.single ? first.front() : UiValue(binding.value.text(first));
            node.scoped_context->set(binding.name, binding.type.empty() ? std::move(value) : value.as(binding.type));
            break;
        }
        case Kind::visible: {
            const bool hide = !first.front().truthy();
            if (node.hidden_by_if != hide) {
                node.hidden_by_if = hide;
                result.layout_changed = true;
            }
            break;
        }
        case Kind::class_toggle: {
            const bool want = binding.compares ? first.front().text() == binding.other.text(second)
                                               : first.front().truthy();
            if (want == node.has_class(binding.name)) break;
            if (want) {
                node.classes.push_back(binding.name);
            } else {
                std::erase(node.classes, binding.name);
            }
            result.layout_changed = true;
            break;
        }
        case Kind::style: {
            // A changed value means a re-resolve of the cascade.
            auto& current = node.inline_styles[binding.name];
            if (std::string text = binding.value.text(first); text != current) {
                current = std::move(text);
                result.layout_changed = true;
            }
            break;
        }
        case Kind::src:
        case Kind::material: {
            // Loading what they name takes the resource manager, which
            // binding does not have: the node is handed back for its owner
            // to load.
            std::string& bound = binding.kind == Kind::src ? node.bound_src : node.bound_material;
            if (std::string text = binding.value.text(first); text != bound) {
                bound = std::move(text);
                if (std::ranges::find(result.asset_changes, &node) == result.asset_changes.end()) {
                    result.asset_changes.push_back(&node);
                }
            }
            break;
        }
        case Kind::text: {
            if (std::string text = binding.value.text(first); text != node.text_content) {
                node.text_content = std::move(text);
                result.text_changed = true;
            }
            break;
        }
        case Kind::bar: {
            // A bound value is read as the number it holds; anything else as
            // the number its text starts with.
            auto number = [](const Template& t, std::span<const UiValue> v, float fallback) {
                const std::optional<float> n = t.single ? v.front().number() : UiValue(t.text(v)).number();
                return n.value_or(fallback);
            };
            const float value = number(binding.value, first, 0.0f);
            float max = number(binding.other, second, 1.0f);
            if (max <= 0.0f) max = 1.0f;
            const float fraction = std::clamp(value / max, 0.0f, 1.0f);
            if (fraction != node.fill_fraction) {
                node.fill_fraction = fraction;
                result.text_changed = true; // a bar's fill is a visual-only change
            }
            break;
        }
        case Kind::repeat:
            break; // repeat() handles its own
    }
}

bool UiDataBind::repeat(Binding& binding, const NodeEntry& entry, DataBindResult& result) {
    ++work_.evaluated;
    UiNode& parent = *entry.node;
    const UiValue listed = entry.scope->lookup(binding.value.parts.front().expression.path);
    const UiList* list = listed.list();
    const std::size_t count = list != nullptr ? list->size() : 0;

    std::size_t at = 0;
    while (at < parent.children.size() && parent.children[at].get() != binding.repeated) ++at;
    if (at == parent.children.size()) return false; // the template is gone: nothing to stamp from
    const std::size_t first = at + 1;
    std::size_t existing = 0; // repeats sit right after their template
    while (first + existing < parent.children.size() && parent.children[first + existing]->foreach_item_index >= 0) {
        ++existing;
    }

    const bool changed = existing != count;
    if (changed) {
        result.layout_changed = true;
        ++work_.applied;
        if (existing > count) {
            parent.children.erase(parent.children.begin() + static_cast<std::ptrdiff_t>(first + count),
                                  parent.children.begin() + static_cast<std::ptrdiff_t>(first + existing));
        }
        for (std::size_t j = existing; j < count; ++j) {
            auto clone = clone_node(*binding.repeated, &parent);
            clone->is_foreach_template = false;
            clone->hidden_by_if = false;
            clone->foreach_item_index = static_cast<int>(j);
            clone->attributes.erase("foreach");
            clone->repeat_scope = std::make_unique<UiContext>(entry.scope);
            parent.children.insert(parent.children.begin() + static_cast<std::ptrdiff_t>(first + j),
                                   std::move(clone));
        }
    }
    // Each repeat's entry is read again every frame: a vector moves its
    // entries when it grows, and a struct entry is read where it is now.
    for (std::size_t j = 0; j < count; ++j) {
        UiContext& scope = *parent.children[first + j]->repeat_scope;
        scope.set(binding.name, list->at(j));
        if (!binding.index.empty()) scope.set(binding.index, UiValue(static_cast<int>(j)));
    }
    return changed;
}

// ── Cloning ─────────────────────────────────────────────────────────────

std::unique_ptr<UiNode> UiDataBind::clone_node(const UiNode& source, UiNode* new_parent) {
    auto clone = std::make_unique<UiNode>();
    clone->type = source.type;
    clone->tag = source.tag;
    clone->id = source.id;
    clone->classes = source.classes;
    clone->attributes = source.attributes;
    clone->text_content = source.text_content;
    clone->text_template = source.text_template;
    clone->parent = new_parent;
    clone->resolved_texture_id = source.resolved_texture_id;
    clone->resolved_material_id = source.resolved_material_id;
    clone->resolved_pipeline_id = source.resolved_pipeline_id;
    clone->fill_fraction = source.fill_fraction;
    clone->fill_display = source.fill_display;
    clone->fill_velocity = source.fill_velocity;
    clone->fill_agitation = source.fill_agitation;
    clone->source_file = source.source_file;
    clone->source_line = source.source_line;

    // Copy widget state
    clone->is_widget = source.is_widget;
    clone->widget_tag = source.widget_tag;
    clone->widget_class = source.widget_class;
    clone->widget_props = source.widget_props;
    clone->widget_models = source.widget_models;
    clone->prop_map = source.prop_map;
    clone->event_map = source.event_map;

    for (const auto& child : source.children) {
        clone->children.push_back(clone_node(*child, clone.get()));
    }
    return clone;
}

} // namespace fjell
