#include "core/input_context_io.hpp"
#include "core/input_source.hpp"
#include "core/log.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fjell {

static ModifierType parse_modifier_type(const std::string& s) {
    if (s == "scale") return ModifierType::scale;
    if (s == "deadzone") return ModifierType::deadzone;
    if (s == "negate") return ModifierType::negate;
    if (s == "swizzle") return ModifierType::swizzle;
    return ModifierType::scale;
}

InputMappingContext parse_input_context(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        FJELL_CORE_WARN("parse_input_context: failed to open {}", path);
        return {};
    }
    std::stringstream text;
    text << f.rdbuf();
    return input_context_from_text(text.str(), path);
}

InputMappingContext input_context_from_text(std::string_view text, const std::string& where) {
    InputMappingContext ctx;
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) {
        FJELL_CORE_WARN("parse_input_context: invalid JSON in {}", where);
        return ctx;
    }

    ctx.name = j.value("name", "");
    ctx.consume = j.value("consume", true);

    if (j.contains("bindings") && j["bindings"].is_array()) {
        for (const auto& bj : j["bindings"]) {
            InputBinding binding;
            binding.action = bj.value("action", "");
            if (binding.action.empty()) continue;

            // Single source
            if (bj.contains("source")) {
                binding.source = input_source_from_string(bj["source"].get<std::string>());
            }

            // Composite WASD-style
            if (bj.contains("up")) binding.up = input_source_from_string(bj["up"].get<std::string>());
            if (bj.contains("down")) binding.down = input_source_from_string(bj["down"].get<std::string>());
            if (bj.contains("left")) binding.left = input_source_from_string(bj["left"].get<std::string>());
            if (bj.contains("right")) binding.right = input_source_from_string(bj["right"].get<std::string>());

            // Modifiers
            if (bj.contains("modifiers") && bj["modifiers"].is_array()) {
                for (const auto& mj : bj["modifiers"]) {
                    InputModifier mod;
                    mod.type = parse_modifier_type(mj.value("type", "scale"));
                    mod.value = mj.value("value", 0.0f);
                    binding.modifiers.push_back(mod);
                }
            }

            ctx.bindings.push_back(std::move(binding));
        }
    }

    return ctx;
}

static const char* modifier_type_string(ModifierType type) {
    switch (type) {
        case ModifierType::scale: return "scale";
        case ModifierType::deadzone: return "deadzone";
        case ModifierType::negate: return "negate";
        case ModifierType::swizzle: return "swizzle";
    }
    return "scale";
}

std::string input_context_text(const InputMappingContext& ctx) {
    nlohmann::json j;
    j["name"] = ctx.name;
    j["consume"] = ctx.consume;

    auto& bindings_arr = j["bindings"];
    bindings_arr = nlohmann::json::array();

    for (const auto& b : ctx.bindings) {
        nlohmann::json bj;
        bj["action"] = b.action;

        if (b.is_composite()) {
            bj["up"] = input_source_to_string(b.up);
            bj["down"] = input_source_to_string(b.down);
            bj["left"] = input_source_to_string(b.left);
            bj["right"] = input_source_to_string(b.right);
        } else {
            bj["source"] = input_source_to_string(b.source);
        }

        if (!b.modifiers.empty()) {
            auto& marr = bj["modifiers"];
            marr = nlohmann::json::array();
            for (const auto& m : b.modifiers) {
                nlohmann::json mj;
                mj["type"] = modifier_type_string(m.type);
                if (m.type != ModifierType::negate && m.type != ModifierType::swizzle) {
                    mj["value"] = m.value;
                }
                marr.push_back(mj);
            }
        }

        bindings_arr.push_back(bj);
    }
    return j.dump(4) + "\n";
}

void write_input_context(const InputMappingContext& ctx, const std::string& path) {
    std::ofstream out(path);
    out << input_context_text(ctx);
}

InputActionDef parse_action_file(const std::string& path) {
    InputActionDef def;

    std::ifstream f(path);
    if (!f.is_open()) return def;

    auto j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) return def;

    def.name = j.value("name", "");
    auto type_str = j.value("type", "button");
    if (type_str == "button") def.type = ActionType::button;
    else if (type_str == "axis1d") def.type = ActionType::axis1d;
    else if (type_str == "axis2d") def.type = ActionType::axis2d;
    else def.type = ActionType::button;

    return def;
}

static const char* action_type_string(ActionType type) {
    switch (type) {
        case ActionType::button: return "button";
        case ActionType::axis1d: return "axis1d";
        case ActionType::axis2d: return "axis2d";
    }
    return "button";
}

// Reads the file back first so fields this format does not model — the
// __uuid asset identity above all — survive a write that only means to
// change the name or the type.
void write_action_file(const InputActionDef& def, const std::string& path) {
    nlohmann::json j;
    if (std::ifstream in(path); in.is_open()) {
        j = nlohmann::json::parse(in, nullptr, false);
    }
    if (!j.is_object()) j = nlohmann::json::object();

    j["name"] = def.name;
    j["type"] = action_type_string(def.type);

    std::ofstream out(path);
    out << j.dump(4) << "\n";
}

std::vector<InputActionDef> scan_action_files(const std::string& dir) {
    namespace fs = std::filesystem;
    std::vector<InputActionDef> result;

    if (!fs::exists(dir)) return result;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".fjaction") {
            auto def = parse_action_file(entry.path().string());
            if (!def.name.empty()) {
                result.push_back(std::move(def));
            }
        }
    }

    return result;
}

} // namespace fjell
