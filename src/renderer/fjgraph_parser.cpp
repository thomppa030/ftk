#include "renderer/fjgraph_parser.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace fjell {

namespace {

constexpr std::array<std::string_view, 6> kPhaseNames{
    "Environment", "DepthPrepass", "GpuDriven", "Color", "PostProcess", "Debug"};

/// Format an error with `path:line` when nlohmann reports a byte offset.
std::string format_error(const std::string& path,
                         const std::string& message,
                         int line = -1) {
    std::ostringstream oss;
    oss << path;
    if (line > 0) { oss << ':' << line; }
    oss << ": " << message;
    return oss.str();
}

/// Translate nlohmann::json::parse_error byte offset into a 1-based line.
/// json.what() includes "at line N" but only when the input had a newline
/// before the error; falling back to the byte offset is more robust.
int line_for_byte(std::string_view text, std::size_t byte_offset) {
    if (byte_offset >= text.size()) { return 1; }
    int line = 1;
    for (std::size_t i = 0; i < byte_offset; ++i) {
        if (text[i] == '\n') { ++line; }
    }
    return line;
}

/// Convert a JSON scalar into our `PassParamValue`. Arrays/objects are
/// rejected — params are intentionally flat in v1.
Result<PassParamValue> param_from_json(const nlohmann::json& v,
                                       const std::string& path,
                                       const std::string& key) {
    if (v.is_string()) { return PassParamValue{v.get<std::string>()}; }
    if (v.is_boolean()) { return PassParamValue{v.get<bool>()}; }
    if (v.is_number_integer() || v.is_number_unsigned()) {
        return PassParamValue{v.get<int64_t>()};
    }
    if (v.is_number_float()) { return PassParamValue{v.get<double>()}; }
    if (v.is_null()) { return PassParamValue{std::monostate{}}; }
    return make_error(format_error(
        path, "param '" + key + "': arrays/objects are not allowed in v1"));
}

bool is_valid_phase(std::string_view name) {
    return std::ranges::any_of(kPhaseNames,
                               [&](std::string_view p) { return p == name; });
}

bool is_valid_queue(std::string_view name) {
    return name == "graphics" || name == "async_compute";
}

} // namespace

Result<FjGraph> FjGraphParser::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return make_error("Failed to open .fjgraph: " + path);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str(), path);
}

Result<FjGraph> FjGraphParser::parse(std::string_view text,
                                     std::string source_path) {
    nlohmann::json j;
    try {
        // ignore_comments=true so authors can annotate `.fjgraph` files.
        // Strict-JSON pipelines would reject these, but `.fjgraph` is an
        // engine-internal asset and readability beats spec compliance.
        j = nlohmann::json::parse(text, /*cb*/ nullptr,
                                  /*allow_exceptions*/ true,
                                  /*ignore_comments*/ true);
    } catch (const nlohmann::json::parse_error& e) {
        const int line = line_for_byte(text, e.byte);
        return make_error(format_error(source_path, e.what(), line));
    }

    if (!j.is_object()) {
        return make_error(format_error(source_path,
            "top-level value must be a JSON object"));
    }

    FjGraph graph;
    graph.source_path = std::move(source_path);
    graph.name = j.value("name", "");

    // ── resources block (optional, informational in v1) ──────────────
    if (j.contains("resources")) {
        const auto& res = j["resources"];
        if (!res.is_object()) {
            return make_error(format_error(graph.source_path,
                "'resources' must be an object"));
        }
        for (const auto& [name, body] : res.items()) {
            if (!body.is_object()) {
                return make_error(format_error(graph.source_path,
                    "resource '" + name + "' must be an object"));
            }
            FjGraphResource r;
            r.name = name;
            r.format = body.value("format", "");
            r.samples = body.value("samples", 1);
            r.mips = body.value("mips", 1);
            r.array_layers = body.value("array_layers", 1);
            r.persistent = body.value("persistent", false);

            // 'size' is permissive: string DSL or [w,h] array.
            if (body.contains("size")) {
                const auto& sz = body["size"];
                if (sz.is_string()) {
                    r.size = sz.get<std::string>();
                } else if (sz.is_array()) {
                    r.size = sz.dump(); // store as "[w,h]"; resolver decides
                } else {
                    return make_error(format_error(graph.source_path,
                        "resource '" + name + "': 'size' must be string or [w,h]"));
                }
            }

            graph.resources.push_back(std::move(r));
        }
    }

    // ── passes block (required) ──────────────────────────────────────
    if (!j.contains("passes")) {
        return make_error(format_error(graph.source_path,
            "missing required 'passes' array"));
    }
    const auto& passes = j["passes"];
    if (!passes.is_array()) {
        return make_error(format_error(graph.source_path,
            "'passes' must be an array"));
    }

    std::unordered_set<std::string> seen_names;
    for (std::size_t i = 0; i < passes.size(); ++i) {
        const auto& entry = passes[i];
        if (!entry.is_object()) {
            return make_error(format_error(graph.source_path,
                "passes[" + std::to_string(i) + "] must be an object"));
        }
        if (!entry.contains("class") || !entry["class"].is_string()) {
            return make_error(format_error(graph.source_path,
                "passes[" + std::to_string(i) + "] missing 'class' string"));
        }

        FjGraphPass p;
        p.class_name = entry["class"].get<std::string>();
        p.name = entry.value("name", std::string{});

        if (entry.contains("params")) {
            const auto& params = entry["params"];
            if (!params.is_object()) {
                return make_error(format_error(graph.source_path,
                    "passes[" + std::to_string(i) + "].params must be an object"));
            }
            for (const auto& [k, v] : params.items()) {
                auto pv = param_from_json(v, graph.source_path, k);
                if (!pv) { return make_error(pv.error()); }
                p.params.set(k, std::move(*pv));
            }
        }

        if (entry.contains("enabled")) {
            p.enabled = entry["enabled"].get<bool>();
        }
        if (entry.contains("queue")) {
            p.queue = entry["queue"].get<std::string>();
        }
        if (entry.contains("phase")) {
            p.phase = entry["phase"].get<std::string>();
        }
        if (entry.contains("sub_order")) {
            p.sub_order = entry["sub_order"].get<int>();
        }
        if (entry.contains("enabled_if")) {
            p.enabled_if = entry["enabled_if"].get<std::string>();
        }

        graph.passes.push_back(std::move(p));
    }

    return validate(std::move(graph));
}

Result<FjGraph> FjGraphParser::validate(FjGraph graph) {
    std::unordered_set<std::string> names_seen;

    for (std::size_t i = 0; i < graph.passes.size(); ++i) {
        auto& p = graph.passes[i];
        const std::string locus = "passes[" + std::to_string(i) + "] ('" +
                                  p.class_name + "')";

        if (!PassFactory::has(p.class_name)) {
            return make_error(format_error(graph.source_path,
                locus + ": unknown pass class — not registered with PassFactory"));
        }

        // Default the registry name from class_name + index when omitted.
        // Two passes of the same class need distinct names; the parser
        // doesn't try to be clever — `.fjgraph` author specifies them.
        if (p.name.empty()) {
            p.name = p.class_name;
        }
        if (!names_seen.insert(p.name).second) {
            return make_error(format_error(graph.source_path,
                locus + ": duplicate pass name '" + p.name +
                "' — set 'name' explicitly to disambiguate"));
        }

        if (p.queue.has_value() && !is_valid_queue(*p.queue)) {
            return make_error(format_error(graph.source_path,
                locus + ": invalid 'queue' value '" + *p.queue +
                "' — expected 'graphics' or 'async_compute'"));
        }
        if (p.phase.has_value() && !is_valid_phase(*p.phase)) {
            return make_error(format_error(graph.source_path,
                locus + ": invalid 'phase' value '" + *p.phase + "'"));
        }
    }

    return graph;
}

} // namespace fjell
