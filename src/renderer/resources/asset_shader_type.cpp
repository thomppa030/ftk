#include "renderer/resources/asset_shader_type.hpp"

#include "core/base_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace fjell {
namespace {

namespace fs = std::filesystem;

// What a file said the last time it was read. A material keeps the reference
// to its shader rather than the Type, so editing the shader alone is noticed.
struct Probe {
    fs::file_time_type time;
    std::optional<ShaderType> type; // a .fjsl's declaration
    std::string shader;             // a .fjmat's shader reference
};

std::mutex g_mutex;
std::unordered_map<std::string, Probe> g_probes;

fs::file_time_type file_time(const fs::path& file) {
    std::error_code ec;
    auto time = fs::last_write_time(file, ec);
    return ec ? fs::file_time_type::min() : time;
}

std::optional<std::string> read_text(const fs::path& file) {
    std::ifstream in(file);
    if (!in.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The shader a .fjmat names, the way load_material_asset() reads it: absent
// means the standard shader. Empty when the file is not a readable material.
std::string read_material_shader(const fs::path& file) {
    std::ifstream in(file);
    if (!in.is_open()) return {};
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) return {};
    auto shader = j.find("shader");
    if (shader == j.end()) return "shaders/standard.fjsl";
    return shader->is_string() ? shader->get<std::string>() : std::string{};
}

// The cached probe for `file`, re-read when the file has changed. Holds the
// lock for the read: two threads asking about one file read it once.
Probe probe(const fs::path& file) {
    const auto time = file_time(file);
    std::lock_guard lock(g_mutex);
    auto& cached = g_probes[file.string()];
    if (cached.time == time && time != fs::file_time_type::min()) return cached;

    cached = Probe{.time = time, .type = std::nullopt, .shader = {}};
    if (file.extension() == ".fjmat") {
        cached.shader = read_material_shader(file);
    } else if (auto text = read_text(file)) {
        cached.type = declared_shader_type(*text);
    }
    return cached;
}

} // namespace

bool carries_shader_type(const fs::path& file) {
    const auto ext = file.extension();
    return ext == ".fjsl" || ext == ".fjmat";
}

std::optional<ShaderType> asset_shader_type(const fs::path& file, const Project* project) {
    if (!carries_shader_type(file)) return std::nullopt;
    auto found = probe(file);
    if (file.extension() == ".fjsl") return found.type;

    if (found.shader.empty()) return std::nullopt;
    auto shader = base_path::resolve_shared(found.shader, project);
    if (shader.empty()) return std::nullopt;
    return probe(shader).type;
}

bool shader_type_accepted(const fs::path& file, const Project* project,
                          std::span<const ShaderType> accepted) {
    if (accepted.empty() || !carries_shader_type(file)) return true;
    auto type = asset_shader_type(file, project);
    return type && std::ranges::find(accepted, *type) != accepted.end();
}

std::span<const ShaderType> only_shader_type(ShaderType type) {
    static constexpr std::array ALL = {ShaderType::Spatial, ShaderType::Sky,   ShaderType::Ui,
                                       ShaderType::Terrain, ShaderType::Water, ShaderType::Fog};
    for (const auto& each : ALL) {
        if (each == type) return {&each, 1};
    }
    return {};
}

std::string describe_shader_types(std::span<const ShaderType> accepted) {
    std::string out;
    for (size_t i = 0; i < accepted.size(); ++i) {
        if (i > 0) out += i + 1 == accepted.size() ? " or " : ", ";
        out += shader_type_name(accepted[i]);
    }
    return out;
}

} // namespace fjell
