#pragma once

#include "renderer/pass/render_pass.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>

namespace fjell {

/// Per-pass instantiation parameters carried in `.fjgraph`.
///
/// `.fjgraph` uses a tiny ad-hoc value type instead of pulling in a JSON
/// library: every parameter we currently need is either a string (for
/// expressions like `enabled_if`) or a small integer (mip index, viz mode).
/// Float and bool exist for forward compat with the resources block.
using PassParamValue = std::variant<std::monostate, std::string, int64_t,
                                    double, bool>;

/// Pass-specific construction params. Keys are strings, values are scalars.
/// The factory closure inspects the keys it cares about and ignores the
/// rest. Missing keys are an error in the factory's hands, not the
/// registry's.
class PassParams {
public:
    void set(std::string key, PassParamValue value) {
        values_[std::move(key)] = std::move(value);
    }

    [[nodiscard]] bool has(const std::string& key) const {
        return values_.contains(key);
    }

    [[nodiscard]] const PassParamValue* find(const std::string& key) const {
        auto it = values_.find(key);
        return it == values_.end() ? nullptr : &it->second;
    }

    /// Read an int64 param, returning `fallback` when absent or wrong-typed.
    [[nodiscard]] int64_t int_or(const std::string& key, int64_t fallback) const {
        const auto* v = find(key);
        if (v == nullptr) { return fallback; }
        if (const auto* i = std::get_if<int64_t>(v)) { return *i; }
        return fallback;
    }

    [[nodiscard]] const std::string* string(const std::string& key) const {
        const auto* v = find(key);
        if (v == nullptr) { return nullptr; }
        return std::get_if<std::string>(v);
    }

    [[nodiscard]] bool empty() const { return values_.empty(); }
    [[nodiscard]] auto begin() const { return values_.begin(); }
    [[nodiscard]] auto end() const { return values_.end(); }

private:
    std::unordered_map<std::string, PassParamValue> values_;
};

/// Maps a class name string → factory closure that produces a `RenderPass`.
///
/// Passes register themselves in their `.cpp` via `FJELL_REGISTER_PASS`.
/// `.fjgraph` parsing then invokes `PassFactory::make("BloomDsPass", params)`
/// to instantiate by string. Keeping registration in the implementation
/// files (not in `pass_setup.cpp`) means adding a pass touches one
/// translation unit instead of two.
class PassFactory {
public:
    using Maker = std::function<std::unique_ptr<RenderPass>(const PassParams&)>;

    /// Register a factory under `class_name`. Idempotent: re-registering
    /// the same name overwrites silently (lets hot-reloaded TUs replace
    /// their factory without restart).
    static void register_class(std::string class_name, Maker maker);

    /// Look up and invoke a factory. Returns null if `class_name` isn't
    /// registered. The caller (parser) is responsible for reporting the
    /// missing class as a user-facing parse error.
    [[nodiscard]] static std::unique_ptr<RenderPass> make(
        const std::string& class_name, const PassParams& params);

    /// True if `class_name` has a registered factory. Used by the parser
    /// to validate references before instantiation.
    [[nodiscard]] static bool has(const std::string& class_name);

    /// Iterate all registered class names (used by the editor to populate
    /// the "add pass" menu and validate `.fjgraph` references).
    [[nodiscard]] static std::vector<std::string> registered_classes();

private:
    static std::unordered_map<std::string, Maker>& registry();
};

/// Auto-registration helper. Construct one at namespace scope per TU; the
/// constructor calls `PassFactory::register_class`. Use the
/// `FJELL_REGISTER_PASS` macro instead of constructing directly so the
/// variable name doesn't collide.
struct PassRegistrar {
    PassRegistrar(std::string class_name, PassFactory::Maker maker) {
        PassFactory::register_class(std::move(class_name), std::move(maker));
    }
};

} // namespace fjell

/// Register a pass class with the factory.
///
/// Default form — class is default-constructible:
///   FJELL_REGISTER_PASS(GtaoTracePass);
///
/// Parameterized form — pass a lambda that consumes `PassParams`:
///   FJELL_REGISTER_PASS_FN(BloomDsPass,
///       [](const PassParams& p) {
///           return std::make_unique<BloomDsPass>(
///               static_cast<uint32_t>(p.int_or("mip_index", 0)));
///       });
#define FJELL_REGISTER_PASS(ClassName)                                          \
    namespace {                                                                 \
    const ::fjell::PassRegistrar fjell_registrar_##ClassName{                   \
        #ClassName,                                                             \
        [](const ::fjell::PassParams&) -> std::unique_ptr<::fjell::RenderPass> {\
            return std::make_unique<ClassName>();                               \
        }};                                                                     \
    }

#define FJELL_REGISTER_PASS_FN(ClassName, Fn)                                   \
    namespace {                                                                 \
    const ::fjell::PassRegistrar fjell_registrar_##ClassName{#ClassName, Fn};   \
    }
