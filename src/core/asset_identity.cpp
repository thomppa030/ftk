#include "core/asset_identity.hpp"

#include "core/log.hpp"
#include "renderer/resources/fjmesh_writer.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <unordered_set>

namespace fjell::asset_identity {

namespace {

constexpr const char* SIDECAR_EXT = ".fjid";

bool is_fjell_json_ext(const std::string& ext) {
    // .fjui, .fjss and .fjwidget are markup and stylesheets, not JSON — they
    // take a sidecar like any other format the engine cannot write a field
    // into.
    static const std::unordered_set<std::string> exts = {
        ".fjmat", ".fjvfx", ".fjanimset", ".fjblend1D", ".fjblend2D",
        ".fjaction", ".fjinput", ".fjanim", ".fjaudio",
        ".fjday", ".fjweather", ".fjclimate",
        ".fjp", ".fjell",
    };
    return exts.contains(ext);
}

bool is_fjell_binary_ext(const std::string& ext) {
    return ext == ".fjmesh" || ext == ".fjskel";
}

AssetId read_json(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in.is_open()) return {};

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(in);
    } catch (const nlohmann::json::parse_error&) {
        return {};
    }

    if (!j.contains(UUID_KEY) || !j[UUID_KEY].is_string()) return {};
    return AssetId::from_string(j[UUID_KEY].get<std::string>());
}

/// Rewrites the file with __uuid inserted, leaving every other field alone.
bool write_json(const std::filesystem::path& file, const AssetId& id) {
    nlohmann::json j;
    {
        std::ifstream in(file);
        if (!in.is_open()) return false;
        try {
            j = nlohmann::json::parse(in);
        } catch (const nlohmann::json::parse_error& e) {
            FJELL_CORE_WARN("Cannot store id in '{}': {}", file.string(), e.what());
            return false;
        }
    }

    if (!j.is_object()) return false;
    j[UUID_KEY] = id.to_string();

    // Atomic write so a crash mid-save can't truncate the asset.
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        if (!out.is_open()) return false;
        out << j.dump(4) << "\n";
        if (!out) return false;
    }

    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

AssetId read_sidecar(const std::filesystem::path& file) {
    std::ifstream in(sidecar_path(file));
    if (!in.is_open()) return {};

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(in);
    } catch (const nlohmann::json::parse_error&) {
        return {};
    }

    if (!j.contains("uuid") || !j["uuid"].is_string()) return {};
    return AssetId::from_string(j["uuid"].get<std::string>());
}

bool write_sidecar(const std::filesystem::path& file, const AssetId& id) {
    std::ofstream out(sidecar_path(file));
    if (!out.is_open()) return false;
    out << "{\"uuid\": \"" << id.to_string() << "\"}\n";
    return static_cast<bool>(out);
}

} // namespace

Carrier carrier_for(const std::string& extension) {
    if (is_fjell_json_ext(extension)) return Carrier::fjell_json;
    if (is_fjell_binary_ext(extension)) return Carrier::fjell_binary;
    return Carrier::sidecar;
}

std::filesystem::path sidecar_path(const std::filesystem::path& file) {
    auto p = file;
    p += SIDECAR_EXT;
    return p;
}

bool is_sidecar(const std::filesystem::path& file) {
    return file.extension() == SIDECAR_EXT;
}

AssetId read(const std::filesystem::path& file) {
    switch (carrier_for(file.extension().string())) {
        case Carrier::fjell_json:
            return read_json(file);
        case Carrier::fjell_binary: {
            auto embedded = AssetId::from_string(read_authored_data(file.string()).asset_uuid);
            if (embedded.valid()) return embedded;
            // Older meshes keep their id in a sidecar until a reimport can
            // store it in the file itself.
            return read_sidecar(file);
        }
        case Carrier::sidecar:
            return read_sidecar(file);
    }
    return {};
}

bool write(const std::filesystem::path& file, const AssetId& id) {
    if (!id.valid()) return false;

    switch (carrier_for(file.extension().string())) {
        case Carrier::fjell_json:
            return write_json(file, id);
        case Carrier::fjell_binary: {
            auto authored = read_authored_data(file.string());
            authored.asset_uuid = id.to_string();
            if (rewrite_authored_data(file.string(), authored)) return true;
            // An older .fjmesh has no room for the field without relaying every
            // section, which only a reimport does. A sidecar carries its
            // identity until then; read() prefers the embedded id once there
            // is one, so the sidecar stops mattering after a reimport.
            return write_sidecar(file, id);
        }
        case Carrier::sidecar:
            return write_sidecar(file, id);
    }
    return false;
}

} // namespace fjell::asset_identity
