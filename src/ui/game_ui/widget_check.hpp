#pragma once

#include "core/result.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
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
    };
    struct Action {
        std::string name;
        std::string param; ///< none, int, float or vec2
    };
    struct Type {
        std::string name;
        std::string kind; ///< component, ui_widget or struct
        std::vector<Field> fields;
        std::vector<Action> actions;

        [[nodiscard]] const Field* field(std::string_view name) const;
        [[nodiscard]] const Action* action(std::string_view name) const;
    };
    struct Pipe {
        std::string name;
        std::size_t args{0};
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

/// Checks a widget file against its game's schema: every path its bindings
/// read, from a prop, a Model, a controller field or a foreach's names through
/// the fields of the types it reaches; every prop type and Model component;
/// every pipe; every action this widget's controllers answer, with its literal
/// arguments; and each widget it places, for the props and events that widget
/// has. Placed widgets are read from `widgets/` beside the file, or beside it
/// when the file is in one. An on:click that no controller in the file answers
/// is an event for whoever places the widget and is checked there.
[[nodiscard]] std::vector<WidgetProblem> check_widget(const std::string& path, const UiSchema& schema);

} // namespace fjell
