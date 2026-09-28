#include "ui/game_ui/widget_check.hpp"

#include "scripting/reflected_types.hpp"
#include "ui/game_ui/ui_actions.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "ui/game_ui/ui_pipes.hpp"
#include "ui/game_ui/widget_asset.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <tuple>

namespace fjell {

namespace fs = std::filesystem;

// ── The schema ──────────────────────────────────────────────────────────

const UiSchema::Field* UiSchema::Type::field(std::string_view name) const {
    for (const auto& f : fields) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

const UiSchema::Action* UiSchema::Type::action(std::string_view name) const {
    for (const auto& a : actions) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

UiSchema UiSchema::from_json(const nlohmann::json& manifest) {
    UiSchema schema;
    schema.reflected = true;
    const auto none = nlohmann::json::array();
    auto read_fields = [&](const nlohmann::json& type) {
        std::vector<Field> fields;
        for (const auto& f : type.value("fields", none)) {
            fields.push_back({.name = f.value("name", ""),
                              .kind = f.value("kind", ""),
                              .type = f.value("struct", ""),
                              .element = f.value("element", ""),
                              .element_type = f.value("element_struct", ""),
                              .prop = f.value("prop", false),
                              .ref = f.value("ref", "")});
        }
        return fields;
    };
    for (const auto& t : manifest.value("types", none)) {
        Type type{.name = t.value("name", ""), .kind = t.value("kind", ""), .fields = read_fields(t), .actions = {}};
        for (const auto& a : t.value("actions", none)) {
            type.actions.push_back({.name = a.value("name", ""), .param = a.value("param", "none")});
        }
        schema.types[type.name] = std::move(type);
    }
    for (const auto& s : manifest.value("structs", none)) {
        Type type{.name = s.value("name", ""), .kind = "struct", .fields = read_fields(s), .actions = {}};
        schema.types[type.name] = std::move(type);
    }
    for (const auto& p : manifest.value("pipes", none)) {
        schema.pipes.push_back({.name = p.value("name", ""), .args = p.value("args", none).size()});
    }
    return schema;
}

Result<UiSchema> UiSchema::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) return make_error("cannot read " + path);
    const auto manifest = nlohmann::json::parse(file, nullptr, false);
    if (manifest.is_discarded()) return make_error(path + " is not valid JSON");
    return from_json(manifest);
}

std::string UiSchema::find_manifest(const std::string& file) {
    std::error_code ec;
    for (fs::path dir = fs::absolute(file, ec).parent_path(); !dir.empty(); dir = dir.parent_path()) {
        const fs::path candidate = dir / "scripts" / "generated" / "manifest.json";
        if (fs::exists(candidate, ec)) return candidate.string();
        if (dir == dir.parent_path()) break;
    }
    return {};
}

const UiSchema::Type* UiSchema::type(std::string_view name) const {
    auto it = types.find(std::string(name));
    return it != types.end() ? &it->second : nullptr;
}

const UiSchema::Pipe* UiSchema::pipe(std::string_view name) const {
    for (const auto& p : pipes) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

// ── Checking ────────────────────────────────────────────────────────────

namespace {

/// What a path reaches, as far as the schema tells: a value kind, an object
/// of a reflected type, or a list. An empty kind knows nothing more, and
/// nothing past it is checked.
struct Reach {
    std::string kind{};
    std::string type{};         ///< an object's type
    std::string element{};      ///< a list's entries
    std::string element_type{};
};

/// A kind markup reads as a value; references and anything fjreflect could
/// not name are not checked past.
bool is_readable(std::string_view kind) {
    return kind == "bool" || kind == "int" || kind == "float" || kind == "string" || kind == "enum" ||
           kind == "vec2" || kind == "vec3" || kind == "vec4" || kind == "color3" || kind == "color4";
}

Reach reach_of(const UiSchema::Field& field) {
    if (field.kind == "struct") return {.kind = "object", .type = field.type};
    if (field.kind == "list") return {.kind = "list", .element = field.element, .element_type = field.element_type};
    if (is_readable(field.kind)) return {.kind = field.kind};
    return {};
}

Reach entry_of(const Reach& list) {
    if (list.element == "struct") return {.kind = "object", .type = list.element_type};
    if (is_readable(list.element)) return {.kind = list.element};
    return {};
}

int components(std::string_view kind) {
    if (kind == "vec2") return 2;
    if (kind == "vec3" || kind == "color3") return 3;
    if (kind == "vec4" || kind == "color4") return 4;
    return 0;
}

int component_index(std::string_view name) {
    if (name == "x" || name == "r") return 0;
    if (name == "y" || name == "g") return 1;
    if (name == "z" || name == "b") return 2;
    if (name == "w" || name == "a") return 3;
    return -1;
}

FjellUiActionParam param_of(std::string_view name) {
    if (name == "int") return FJELL_UI_ACTION_INT;
    if (name == "float") return FJELL_UI_ACTION_FLOAT;
    if (name == "vec2") return FJELL_UI_ACTION_VEC2;
    return FJELL_UI_ACTION_NONE;
}

bool is_value_kind(std::string_view type) {
    return type == "float" || type == "int" || type == "bool" || type == "string";
}

/// One scope: the names it holds, then the controller whose fields it reads.
struct Frame {
    std::unordered_map<std::string, Reach> names;
    const UiSchema::Type* controller{nullptr};
    /// A controller the schema does not describe: any name may be its field.
    bool unknown_controller{false};
};

class Checker {
public:
    Checker(std::string path, const UiSchema& schema) : path_(std::move(path)), schema_(schema) {}

    WidgetCheck run();

private:
    void walk(const UiNode& node);
    Reach check_path(const UiNode& node, std::string_view path, std::string_view needle);
    Reach check_expression(const UiNode& node, std::string_view text);
    Reach check_template(const UiNode& node, std::string_view text);
    void check_placement(const UiNode& node, const WidgetAsset& placed);
    void check_prop(const UiNode& node, const std::string& name, const std::string& value, const WidgetAsset& placed);
    /// A call written in this file: its bound arguments, and whether the
    /// controller that answers it takes what it is given. False when no
    /// controller here answers it.
    bool check_call(const UiNode& node, const std::string& text, const UiSchema::Type* inner);
    /// The action a call reaches: `inner`'s, else the nearest controller's
    /// around it that declares one by that name.
    [[nodiscard]] const UiSchema::Action* answering(std::string_view name, const UiSchema::Type* inner) const;
    [[nodiscard]] std::optional<Reach> lookup(std::string_view name) const;
    /// Whether every controller a call could reach is known, so that one
    /// none of them answers is a mistake.
    [[nodiscard]] bool knows_every_action(const UiSchema::Type* inner, const WidgetAsset* placed) const;
    [[nodiscard]] Reach prop_reach(const PropDecl& prop);
    /// Remembers the type a reach is or holds: the check's answer depends on it.
    void note(const Reach& reach);
    void note_type(const std::string& type);
    const WidgetAsset* placed_widget(const std::string& tag);
    void report(const UiNode& node, std::string_view needle, std::string message);
    void report_in_file(std::string_view needle, std::string message);
    void read_lines();

    std::string path_;
    const UiSchema& schema_;
    std::vector<std::string> lines_;
    std::vector<Frame> frames_;
    /// A widget in a widgets/ folder is placed by others, and a call no
    /// controller in it answers may be answered around where it is placed.
    bool placed_by_others_{false};
    std::unordered_map<std::string, std::unique_ptr<WidgetAsset>> placed_;
    std::vector<WidgetProblem> problems_;
    std::set<std::string> types_;
    std::set<std::string> pipes_;
    std::vector<std::string> placed_files_;
};

WidgetCheck Checker::run() {
    auto asset = load_widget_asset(path_);
    if (!asset) {
        problems_.push_back({.file = path_, .line = 1, .column = 1, .message = asset.error()});
        return {.problems = std::move(problems_), .types = {}, .pipes = {}, .placed = {}};
    }
    read_lines();

    Frame root;
    for (const auto& prop : asset->props) root.names[prop.name] = prop_reach(prop);
    for (const auto& model : asset->models) {
        note_type(model.component);
        const UiSchema::Type* type = schema_.type(model.component);
        if (type == nullptr || type->kind != "component") {
            if (schema_.reflected) {
                report_in_file("component=\"" + model.component + "\"",
                               "model '" + model.name + "' reads '" + model.component +
                                   "', which is not a script component the game has");
            }
            root.names[model.name] = {};
        } else {
            root.names[model.name] = {.kind = "object", .type = model.component};
        }
    }
    if (!asset->class_name.empty()) {
        note_type(asset->class_name);
        root.controller = schema_.type(asset->class_name);
        if (root.controller == nullptr || root.controller->kind != "ui_widget") {
            if (schema_.reflected) {
                report_in_file("class=\"" + asset->class_name + "\"",
                               "controller '" + asset->class_name + "' is not a UI widget the game has");
            }
            root.controller = nullptr;
            root.unknown_controller = true;
        }
    }
    // A controller's [[fj::prop]] is filled from the widget's prop of its
    // name, of its kind.
    if (root.controller != nullptr) {
        for (const auto& field : root.controller->fields) {
            if (!field.prop) continue;
            const auto declared = std::ranges::find(asset->props, field.name, &PropDecl::name);
            if (declared == asset->props.end()) {
                report_in_file("class=\"" + asset->class_name + "\"",
                               "the controller's [[fj::prop]] '" + field.name + "' has no <prop name=\"" +
                                   field.name + "\"> to be filled from");
            } else if (const bool ref = field.kind == "ref"; declared->type != (ref ? field.ref : field.kind)) {
                report_in_file("name=\"" + field.name + "\"",
                               "<prop> '" + field.name + "' is " + declared->type +
                                   ", but the controller's [[fj::prop]] of that name is " +
                                   (ref ? "Ref<" + field.ref + ">" : field.kind));
            }
        }
    }
    placed_by_others_ = fs::path(path_).parent_path().filename() == "widgets";
    frames_.push_back(std::move(root));
    if (asset->markup_root) walk(*asset->markup_root);

    std::ranges::sort(problems_, [](const WidgetProblem& a, const WidgetProblem& b) {
        return std::tie(a.line, a.column, a.message) < std::tie(b.line, b.column, b.message);
    });
    problems_.erase(std::unique(problems_.begin(), problems_.end(),
                                [](const WidgetProblem& a, const WidgetProblem& b) {
                                    return a.line == b.line && a.column == b.column && a.message == b.message;
                                }),
                    problems_.end());
    return {.problems = std::move(problems_),
            .types = {types_.begin(), types_.end()},
            .pipes = {pipes_.begin(), pipes_.end()},
            .placed = std::move(placed_files_)};
}

void Checker::note(const Reach& reach) {
    if (reach.kind == "object") note_type(reach.type);
    if (reach.kind == "list") note_type(reach.element_type);
}

void Checker::note_type(const std::string& type) {
    if (!type.empty()) types_.insert(type);
}

Reach Checker::prop_reach(const PropDecl& prop) {
    if (is_value_kind(prop.type)) return {.kind = prop.type};
    note_type(prop.type);
    if (schema_.type(prop.type) != nullptr) return {.kind = "object", .type = prop.type};
    if (!schema_.reflected) return {};
    report_in_file("name=\"" + prop.name + "\"",
                   "prop '" + prop.name + "' has type '" + prop.type +
                       "', which is neither float, int, bool, string nor a type the game reflects");
    return {};
}

void Checker::walk(const UiNode& node) {
    // A repeat's names hold for the node that carries the foreach and all
    // that is in it.
    bool repeated = false;
    if (auto it = node.attributes.find("foreach"); it != node.attributes.end()) {
        const auto spec = parse_foreach(it->second);
        if (!spec) {
            report(node, it->second, "foreach reads `item in list` or `item, i in list`");
        } else {
            const Reach list = check_path(node, spec->list, spec->list);
            if (!list.kind.empty() && list.kind != "list") {
                report(node, spec->list, "'" + spec->list + "' is not a list, so foreach cannot repeat over it");
            }
            Frame repeat;
            repeat.names[spec->item] = list.kind == "list" ? entry_of(list) : Reach{};
            if (!spec->index.empty()) repeat.names[spec->index] = {.kind = "int"};
            frames_.push_back(std::move(repeat));
            repeated = true;
        }
    }

    const bool places = node.type == UiNodeType::element && !is_builtin_tag(node.tag);
    const WidgetAsset* placed = places ? placed_widget(node.tag) : nullptr;
    if (places && placed == nullptr) {
        report(node, "<" + node.tag, "<" + node.tag + "> is neither an element the engine draws nor a widget in " +
                                         "widgets/" + widget_file_name(node.tag) + ".fjwidget");
    }
    for (const auto& [name, value] : node.attributes) {
        if (name == "foreach") continue;
        if (name == "if") {
            check_expression(node, value);
        } else if (name.starts_with("class:")) {
            if (const auto eq = value.find("=="); eq != std::string::npos) {
                check_expression(node, std::string_view(value).substr(0, eq));
                check_template(node, std::string_view(value).substr(eq + 2));
            } else {
                check_expression(node, value);
            }
        } else if (name == "on:click") {
            if (!check_call(node, value, nullptr) && !placed_by_others_ && knows_every_action(nullptr, nullptr)) {
                report(node, value, "no controller around it has an [[fj::action]] named '" +
                                        parse_action_call(value).name + "'");
            }
        } else if (name.starts_with("on:")) {
            if (placed == nullptr) report(node, name, "only a placed widget raises '" + name.substr(3) + "'");
        } else if (placed != nullptr && !name.starts_with("style-")) {
            check_prop(node, name, value, *placed);
        } else {
            check_template(node, value);
        }
    }
    if (node.type == UiNodeType::text) check_template(node, node.text_content);
    if (placed != nullptr) check_placement(node, *placed);

    // A placed widget's own markup is checked with its own file; what is
    // written between its tags here is this file's.
    for (const auto& child : node.children) walk(*child);
    if (repeated) frames_.pop_back();
}

std::optional<Reach> Checker::lookup(std::string_view name) const {
    for (auto frame = frames_.rbegin(); frame != frames_.rend(); ++frame) {
        if (auto it = frame->names.find(std::string(name)); it != frame->names.end()) return it->second;
        if (frame->controller != nullptr) {
            if (const auto* field = frame->controller->field(name)) return reach_of(*field);
        }
        if (frame->unknown_controller) return Reach{};
    }
    return std::nullopt;
}

Reach Checker::check_path(const UiNode& node, std::string_view path, std::string_view needle) {
    if (path.empty()) return {};
    std::vector<std::string_view> steps;
    for (std::size_t start = 0;;) {
        const auto dot = path.find('.', start);
        steps.push_back(path.substr(start, dot - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    std::optional<Reach> at = lookup(steps.front());
    if (!at) {
        // A prop may have a dot in its name.
        if (steps.size() > 1) {
            if (auto whole = lookup(path)) return *whole;
        }
        report(node, needle,
               "nothing here is named '" + std::string(steps.front()) +
                   "': not a prop, a Model, a controller field or a foreach's name");
        return {};
    }
    note(*at);
    std::string walked(steps.front());
    for (std::size_t i = 1; i < steps.size(); ++i) {
        const std::string step(steps[i]);
        if (at->kind == "object") {
            const UiSchema::Type* type = schema_.type(at->type);
            if (type == nullptr) return {};
            const UiSchema::Field* field = type->field(step);
            if (field == nullptr) {
                report(node, needle, "'" + type->name + "' has no field '" + step + "'");
                return {};
            }
            at = reach_of(*field);
            note(*at);
        } else if (const int n = components(at->kind); n > 0) {
            const int c = component_index(step);
            if (c < 0 || c >= n) {
                report(node, needle, "'" + walked + "' is a " + at->kind + ", which has no component '" + step + "'");
                return {};
            }
            at = Reach{.kind = "float"};
        } else if (at->kind.empty()) {
            return {};
        } else if (at->kind == "list") {
            report(node, needle, "'" + walked + "' is a list: repeat it with foreach to read '" + step + "'");
            return {};
        } else {
            report(node, needle, "'" + walked + "' is " + (at->kind == "int" ? "an " : "a ") + at->kind +
                                     ", which has no field '" + step + "'");
            return {};
        }
        walked += "." + step;
    }
    return *at;
}

Reach Checker::check_expression(const UiNode& node, std::string_view text) {
    const UiExpression expression = parse_expression(text);
    Reach reach = check_path(node, expression.path, text);
    for (const auto& call : expression.pipes) {
        if (UiPipes::is_engine_pipe(call.name)) continue;
        pipes_.insert(call.name);
        if (!schema_.reflected) continue;
        const UiSchema::Pipe* pipe = schema_.pipe(call.name);
        if (pipe == nullptr) {
            report(node, text, "no pipe named '" + call.name + "' in the engine or the game");
        } else if (pipe->args != call.args.size()) {
            report(node, text, "|" + call.name + " takes " + std::to_string(pipe->args) + " argument(s), given " +
                                   std::to_string(call.args.size()));
        }
    }
    if (!expression.pipes.empty()) reach = {.kind = "string"}; // a pipe gives text
    return reach;
}

Reach Checker::check_template(const UiNode& node, std::string_view text) {
    Reach single;
    int expressions = 0;
    bool literal = false;
    for (std::size_t i = 0; i < text.size();) {
        const auto end = text[i] == '{' ? text.find('}', i + 1) : std::string_view::npos;
        if (end == std::string_view::npos) {
            if (text[i] != ' ' && text[i] != '\n' && text[i] != '\t') literal = true;
            ++i;
            continue;
        }
        single = check_expression(node, text.substr(i + 1, end - i - 1));
        ++expressions;
        i = end + 1;
    }
    if (expressions == 1 && !literal) return single;
    return {.kind = "string"};
}

const UiSchema::Action* Checker::answering(std::string_view name, const UiSchema::Type* inner) const {
    if (inner != nullptr) {
        if (const auto* action = inner->action(name)) return action;
    }
    for (auto frame = frames_.rbegin(); frame != frames_.rend(); ++frame) {
        if (frame->controller == nullptr) continue;
        if (const auto* action = frame->controller->action(name)) return action;
    }
    return nullptr;
}

bool Checker::knows_every_action(const UiSchema::Type* inner, const WidgetAsset* placed) const {
    if (!schema_.reflected) return false;
    if (placed != nullptr && !placed->class_name.empty() && inner == nullptr) return false;
    return std::ranges::none_of(frames_, &Frame::unknown_controller);
}

bool Checker::check_call(const UiNode& node, const std::string& text, const UiSchema::Type* inner) {
    const UiActionCall call = parse_action_call(text);
    for (const auto& arg : call.args) check_template(node, arg);
    const UiSchema::Action* action = answering(call.name, inner);
    if (action == nullptr) return false;
    OwnedUiActionDesc desc;
    desc.name = action->name;
    desc.param = param_of(action->param);
    if (const std::string why = check_action_call(desc, call); !why.empty()) report(node, text, why);
    return true;
}

void Checker::check_placement(const UiNode& node, const WidgetAsset& placed) {
    // What a click inside the placed widget raises: the text of its on:click,
    // which the placement maps with on:<text> to a call of its own. The
    // mapped call, and one left unmapped that the widget's controller does
    // not answer, go to the widget's controller first, then outward from
    // the placement.
    note_type(placed.class_name);
    for (const auto& prop : placed.props) {
        if (!is_value_kind(prop.type)) note_type(prop.type);
    }
    const UiSchema::Type* inner = placed.class_name.empty() ? nullptr : schema_.type(placed.class_name);
    std::vector<std::string> raised;
    auto collect = [&](auto& self, const UiNode& at) -> void {
        if (auto it = at.attributes.find("on:click"); it != at.attributes.end()) {
            if (std::ranges::find(raised, it->second) == raised.end()) raised.push_back(it->second);
        }
        for (const auto& child : at.children) self(self, *child);
    };
    if (placed.markup_root) collect(collect, *placed.markup_root);

    for (const auto& [name, call] : node.attributes) {
        if (!name.starts_with("on:") || name == "on:click") continue;
        const std::string event = name.substr(3);
        if (std::ranges::find(raised, event) == raised.end()) {
            report(node, name, "<" + node.tag + "> raises no event '" + event + "'");
        }
        if (!check_call(node, call, inner) && !placed_by_others_ && knows_every_action(inner, &placed)) {
            report(node, call, "no controller around <" + node.tag + "> has an [[fj::action]] named '" +
                                   parse_action_call(call).name + "'");
        }
    }
    if (placed_by_others_ || !knows_every_action(inner, &placed)) return;
    for (const auto& event : raised) {
        if (node.attributes.contains("on:" + event)) continue;
        if (answering(parse_action_call(event).name, inner) != nullptr) continue;
        report(node, "<" + node.tag, "<" + node.tag + "> raises '" + event +
                                         "', which no controller around it answers: map it with on:" + event);
    }
}

void Checker::check_prop(const UiNode& node, const std::string& name, const std::string& value,
                         const WidgetAsset& placed) {
    const Reach given = check_template(node, value);
    const auto declared = std::ranges::find(placed.props, name, &PropDecl::name);
    if (declared == placed.props.end()) {
        if (!placed.props.empty()) report(node, name + "=", "<" + node.tag + "> has no prop '" + name + "'");
        return;
    }
    if (given.kind.empty() || declared->type == "string" || declared->type == "bool") return;
    const bool number = given.kind == "int" || given.kind == "float" || given.kind == "bool" || given.kind == "string";
    const std::string given_name = given.kind == "object" ? given.type : given.kind;
    if (declared->type == "int" || declared->type == "float") {
        if (!number) {
            report(node, name + "=", "prop '" + name + "' takes a number, given " + given_name);
        }
    } else if (given.kind != "object" || given.type != declared->type) {
        report(node, name + "=", "prop '" + name + "' takes " + declared->type + ", given " + given_name);
    }
}

const WidgetAsset* Checker::placed_widget(const std::string& tag) {
    if (auto it = placed_.find(tag); it != placed_.end()) return it->second.get();
    const std::string file = widget_file_name(tag) + ".fjwidget";
    const fs::path dir = fs::path(path_).parent_path();
    std::unique_ptr<WidgetAsset> found;
    for (const fs::path& candidate : {dir / "widgets" / file, dir / file}) {
        std::error_code ec;
        if (candidate == fs::path(path_) || !fs::exists(candidate, ec)) continue;
        if (candidate.parent_path() == dir && dir.filename() != "widgets") continue;
        if (auto asset = load_widget_asset(candidate.string())) {
            found = std::make_unique<WidgetAsset>(std::move(*asset));
            placed_files_.push_back(candidate.string());
            break;
        }
    }
    return (placed_[tag] = std::move(found)).get();
}

void Checker::read_lines() {
    std::ifstream file(path_);
    for (std::string line; std::getline(file, line);) lines_.push_back(std::move(line));
}

void Checker::report(const UiNode& node, std::string_view needle, std::string message) {
    // The node's line is where its tag starts; what it says may continue
    // on the lines after.
    const uint32_t first = std::max<uint32_t>(node.source_line, 1);
    for (uint32_t line = first; line <= lines_.size() && line < first + 16; ++line) {
        if (const auto at = lines_[line - 1].find(needle); !needle.empty() && at != std::string::npos) {
            problems_.push_back({.file = path_, .line = line, .column = static_cast<uint32_t>(at) + 1,
                                 .message = std::move(message)});
            return;
        }
    }
    problems_.push_back({.file = path_, .line = first, .column = 1, .message = std::move(message)});
}

void Checker::report_in_file(std::string_view needle, std::string message) {
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (const auto at = lines_[i].find(needle); at != std::string::npos) {
            problems_.push_back({.file = path_, .line = static_cast<uint32_t>(i) + 1,
                                 .column = static_cast<uint32_t>(at) + 1, .message = std::move(message)});
            return;
        }
    }
    problems_.push_back({.file = path_, .line = 1, .column = 1, .message = std::move(message)});
}

} // namespace

WidgetCheck check_widget(const std::string& path, const UiSchema& schema) {
    return Checker(path, schema).run();
}

std::vector<WidgetProblem> WidgetChecks::reload(const std::string& manifest, std::span<const std::string> widgets) {
    UiSchema schema;
    std::error_code ec;
    if (!manifest.empty() && fs::exists(manifest, ec)) {
        if (auto loaded = UiSchema::load(manifest)) {
            schema = std::move(*loaded);
        } else {
            return {{.file = manifest, .line = 1, .column = 1, .message = loaded.error()}};
        }
    }

    // What the new build declares differently: every name either side has
    // that the other has not, or has otherwise.
    std::set<std::string> changed_types;
    for (const auto& [name, type] : schema.types) {
        if (const auto* before = schema_.type(name); before == nullptr || !(*before == type)) {
            changed_types.insert(name);
        }
    }
    for (const auto& [name, type] : schema_.types) {
        if (schema.type(name) == nullptr) changed_types.insert(name);
    }
    std::set<std::string> changed_pipes;
    for (const auto& pipe : schema.pipes) {
        if (const auto* before = schema_.pipe(pipe.name); before == nullptr || !(*before == pipe)) {
            changed_pipes.insert(pipe.name);
        }
    }
    for (const auto& pipe : schema_.pipes) {
        if (schema.pipe(pipe.name) == nullptr) changed_pipes.insert(pipe.name);
    }

    // The first load, or a game built or unbuilt since, answers for every
    // widget anew.
    const bool everything = !loaded_ || schema.reflected != schema_.reflected;
    const bool changed = everything || !changed_types.empty() || !changed_pipes.empty();
    schema_ = std::move(schema);
    loaded_ = true;
    if (!changed) return {};
    ++revision_;

    std::vector<WidgetProblem> problems;
    for (const auto& widget : widgets) {
        WidgetCheck result = check(widget);
        const bool affected = everything || std::ranges::any_of(result.types, [&](const std::string& t) {
                                  return changed_types.contains(t);
                              }) || std::ranges::any_of(result.pipes, [&](const std::string& p) {
                                  return changed_pipes.contains(p);
                              });
        if (!affected) continue;
        std::ranges::move(result.problems, std::back_inserter(problems));
    }
    return problems;
}

} // namespace fjell
