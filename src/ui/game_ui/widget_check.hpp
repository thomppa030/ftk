#pragma once

#include "core/result.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fjell {

/// What a game declares that its widgets can reach, as fjreflect writes it to
/// `scripts/generated/manifest.json`: its components, its widget controllers,
/// the structs they hold, and its pipes.
struct UiSchema {
    /// A field or a getter: what reading it gives.
    struct Field {
        std::string name;
        std::string kind;         ///< bool, int, float, string, vec2…color4, enum, struct, list, …
        std::string type;         ///< a struct field's struct
        std::string element;      ///< a list's entries: their kind
        std::string element_type; ///< and their struct, for a list of structs

        bool operator==(const Field&) const = default;
    };
    struct Action {
        std::string name;
        std::string param; ///< none, int, float or vec2

        bool operator==(const Action&) const = default;
    };
    struct Type {
        std::string name;
        std::string kind; ///< component, ui_widget or struct
        std::vector<Field> fields;
        std::vector<Action> actions;

        [[nodiscard]] const Field* field(std::string_view name) const;
        [[nodiscard]] const Action* action(std::string_view name) const;

        bool operator==(const Type&) const = default;
    };
    struct Pipe {
        std::string name;
        std::size_t args{0};

        bool operator==(const Pipe&) const = default;
    };

    std::unordered_map<std::string, Type> types;
    std::vector<Pipe> pipes;
    /// False for a schema read from no manifest: a game that has not been
    /// built. Its controllers, components, types, pipes and actions are then
    /// unknown, and nothing is reported for naming them.
    bool reflected{false};

    [[nodiscard]] static UiSchema from_json(const nlohmann::json& manifest);
    /// The manifest file, read; an error when it cannot be.
    [[nodiscard]] static Result<UiSchema> load(const std::string& path);
    /// `scripts/generated/manifest.json` in the nearest folder at or above
    /// the file's own that has one; empty when none does.
    [[nodiscard]] static std::string find_manifest(const std::string& file);

    [[nodiscard]] const Type* type(std::string_view name) const;
    [[nodiscard]] const Pipe* pipe(std::string_view name) const;
};

/// One thing a widget file names that its game does not declare.
struct WidgetProblem {
    std::string file;
    uint32_t line{0};
    uint32_t column{0};
    std::string message;
};

/// What checking one widget file found.
struct WidgetCheck {
    std::vector<WidgetProblem> problems;
    /// Every reflected type and game pipe the file names or reaches, whether
    /// the schema has it or not: what the answer depends on.
    std::vector<std::string> types;
    std::vector<std::string> pipes;
    /// The widget files it places, as found.
    std::vector<std::string> placed;
};

/// Checks a widget file against its game's schema: every path its bindings
/// read, from a prop, a Model, a controller field or a foreach's names through
/// the fields of the types it reaches; every prop type and Model component;
/// every pipe; every action this widget's controllers answer, with its literal
/// arguments; and each widget it places, for the props and events that widget
/// has. Placed widgets are read from `widgets/` beside the file, or beside it
/// when the file is in one. An on:click that no controller in the file answers
/// is an event for whoever places the widget and is checked there.
[[nodiscard]] WidgetCheck check_widget(const std::string& path, const UiSchema& schema);

/// A project's widgets checked against its game as it is built now. Reloaded
/// whenever the game library is, it tells which widgets the new build broke
/// or mended.
class WidgetChecks {
public:
    /// Reads the manifest again (none, for a game not built: `manifest` empty
    /// or missing) and checks every widget given against it. Returns the
    /// problems of the widgets that name a type or game pipe the manifest now
    /// declares differently; of every widget the first time.
    std::vector<WidgetProblem> reload(const std::string& manifest, std::span<const std::string> widgets);

    /// One widget file's problems against the schema last loaded.
    [[nodiscard]] WidgetCheck check(const std::string& widget) const { return check_widget(widget, schema_); }

    /// Moves whenever a reload changes the schema, so a view listing
    /// problems knows to check again.
    [[nodiscard]] uint32_t revision() const { return revision_; }

private:
    UiSchema schema_;
    uint32_t revision_{0};
    bool loaded_{false};
};

} // namespace fjell
