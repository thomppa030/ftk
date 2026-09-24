#include "renderer/resources/fjterrain_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace fjell {

namespace {

struct FileCloser {
    void operator()(std::FILE* f) const noexcept {
        if (f != nullptr) std::fclose(f);
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

// Heights live on disk normalized to [0,1]. unorm16 halves the file size and
// gives ~1/65535 of the height range per step, which is finer than terrain
// authoring resolves; float32 exists for cases that need more.
constexpr float UNORM16_MAX = 65535.0F;

// Bounds on the name table, so a corrupt file fails cleanly instead of asking
// for gigabytes.
constexpr uint32_t MAX_NAMES = 1024;
constexpr uint32_t MAX_NAME_LENGTH = 4096;

// A sample grid must be one greater than a whole number of patch quads, so
// patches tile the terrain and share edge rows.
[[nodiscard]] bool patches_tile_evenly(const FjTerrainHeader& h) {
    if (h.sample_count < 2 || h.patch_quads == 0) return false;
    const uint32_t quads = h.sample_count - 1;
    return (quads % h.patch_quads) == 0;
}

} // namespace

Result<FjTerrainData> load_fjterrain(const std::string& path) {
    FilePtr file{std::fopen(path.c_str(), "rb")};
    if (!file) {
        return make_error("fjterrain: cannot open " + path);
    }

    FjTerrainData data;
    if (std::fread(&data.header, sizeof(data.header), 1, file.get()) != 1) {
        return make_error("fjterrain: truncated header in " + path);
    }
    if (data.header.magic != FJTERRAIN_MAGIC) {
        return make_error("fjterrain: bad magic in " + path);
    }
    if (data.header.version != 1 && data.header.version != 2 &&
        data.header.version != FJTERRAIN_VERSION) {
        return make_error("fjterrain: unsupported version " +
                          std::to_string(data.header.version) + " in " + path);
    }
    if (data.header.version == 1 && data.header.layer_count > 0) {
        return make_error("fjterrain: version 1 file carries layers but no name table in " +
                          path);
    }
    // Before the tiling test, because a wrong patch_quads makes that message
    // describe the wrong problem. The mesh shaders declare their output at
    // compile time and step the grid at FJTERRAIN_PATCH_QUADS whatever the
    // file says, while physics and every CPU query use the file's value: a
    // mismatch renders one surface and collides against another. Refusing the
    // file is the only honest answer.
    if (data.header.patch_quads != FJTERRAIN_PATCH_QUADS) {
        return make_error("fjterrain: patch_quads is " +
                          std::to_string(data.header.patch_quads) + " but the renderer only "
                          "draws " + std::to_string(FJTERRAIN_PATCH_QUADS) +
                          "; regenerate the terrain with that patch size, in " + path);
    }
    if (!patches_tile_evenly(data.header)) {
        return make_error("fjterrain: sample_count " +
                          std::to_string(data.header.sample_count) +
                          " is not one greater than a multiple of patch_quads " +
                          std::to_string(data.header.patch_quads) + " in " + path);
    }
    if (data.header.layer_count > FJTERRAIN_MAX_LAYERS) {
        return make_error("fjterrain: layer_count exceeds " +
                          std::to_string(FJTERRAIN_MAX_LAYERS) + " in " + path);
    }

    const size_t samples =
        static_cast<size_t>(data.header.sample_count) * data.header.sample_count;
    data.heights.resize(samples);

    if (data.header.height_format == FjTerrainHeightFormat::unorm16) {
        std::vector<uint16_t> raw(samples);
        if (std::fread(raw.data(), sizeof(uint16_t), samples, file.get()) != samples) {
            return make_error("fjterrain: truncated height data in " + path);
        }
        for (size_t i = 0; i < samples; ++i) {
            data.heights[i] = static_cast<float>(raw[i]) / UNORM16_MAX;
        }
    } else {
        if (std::fread(data.heights.data(), sizeof(float), samples, file.get()) != samples) {
            return make_error("fjterrain: truncated height data in " + path);
        }
    }

    if (data.header.layer_count > 0 && data.header.weight_count > 0) {
        const size_t weights =
            static_cast<size_t>(data.header.weight_count) * data.header.weight_count * 4;
        data.weights.resize(weights);
        if (std::fread(data.weights.data(), 1, weights, file.get()) != weights) {
            return make_error("fjterrain: truncated weight data in " + path);
        }
    }

    if (data.header.layer_count > 0) {
        data.layers.resize(data.header.layer_count);
        if (data.header.version == 2) {
            std::vector<FjTerrainLayerV2> old_layers(data.layers.size());
            if (std::fread(old_layers.data(), sizeof(FjTerrainLayerV2), old_layers.size(),
                           file.get()) != old_layers.size()) {
                return make_error("fjterrain: truncated layer table in " + path);
            }
            // A version 2 layer has no surface; it gets an empty name of its
            // own so every layer keeps one entry per field.
            for (size_t i = 0; i < old_layers.size(); ++i) {
                data.layers[i].material_path = old_layers[i].material_path;
                data.layers[i].uv_scale = old_layers[i].uv_scale;
            }
        } else if (std::fread(data.layers.data(), sizeof(FjTerrainLayer), data.layers.size(),
                              file.get()) != data.layers.size()) {
            return make_error("fjterrain: truncated layer table in " + path);
        }

        uint32_t name_count = 0;
        if (std::fread(&name_count, sizeof(name_count), 1, file.get()) != 1) {
            return make_error("fjterrain: truncated name table in " + path);
        }
        if (name_count > MAX_NAMES) {
            return make_error("fjterrain: name table claims " + std::to_string(name_count) +
                              " entries in " + path);
        }
        data.names.resize(name_count);
        for (auto& name : data.names) {
            uint32_t length = 0;
            if (std::fread(&length, sizeof(length), 1, file.get()) != 1 ||
                length > MAX_NAME_LENGTH) {
                return make_error("fjterrain: truncated name table in " + path);
            }
            name.resize(length);
            if (length > 0 && std::fread(name.data(), 1, length, file.get()) != length) {
                return make_error("fjterrain: truncated name table in " + path);
            }
        }
        if (data.header.version == 2) {
            for (auto& layer : data.layers) {
                layer.surface_path = static_cast<uint32_t>(data.names.size());
                data.names.emplace_back();
            }
        }
        for (const auto& layer : data.layers) {
            if (layer.material_path >= data.names.size() ||
                layer.surface_path >= data.names.size()) {
                return make_error("fjterrain: layer names a string outside the name table in " +
                                  path);
            }
        }
    }

    return data;
}

Result<> save_fjterrain(const std::string& path, const FjTerrainData& data) {
    const uint32_t samples_per_side = data.header.sample_count;
    const size_t samples = static_cast<size_t>(samples_per_side) * samples_per_side;
    if (data.heights.size() != samples) {
        return make_error("fjterrain: height array holds " +
                          std::to_string(data.heights.size()) + " entries, expected " +
                          std::to_string(samples));
    }
    if (data.header.patch_quads != FJTERRAIN_PATCH_QUADS) {
        return make_error("fjterrain: patch_quads is " +
                          std::to_string(data.header.patch_quads) + " but the renderer only "
                          "draws " + std::to_string(FJTERRAIN_PATCH_QUADS));
    }
    if (!patches_tile_evenly(data.header)) {
        return make_error("fjterrain: sample_count is not one greater than a multiple "
                          "of patch_quads");
    }

    FilePtr file{std::fopen(path.c_str(), "wb")};
    if (!file) {
        return make_error("fjterrain: cannot write " + path);
    }

    for (const auto& layer : data.layers) {
        if (layer.material_path >= data.names.size() || layer.surface_path >= data.names.size()) {
            return make_error("fjterrain: layer names a string outside the name table");
        }
    }

    FjTerrainHeader header = data.header;
    header.magic = FJTERRAIN_MAGIC;
    header.version = FJTERRAIN_VERSION;
    header.layer_count = static_cast<uint32_t>(data.layers.size());
    if (data.weights.empty()) {
        header.weight_count = 0;
    }
    const size_t expected_weights =
        static_cast<size_t>(header.weight_count) * header.weight_count * 4;
    if (data.weights.size() != expected_weights) {
        return make_error("fjterrain: weight array holds " +
                          std::to_string(data.weights.size()) + " bytes, expected " +
                          std::to_string(expected_weights));
    }

    if (std::fwrite(&header, sizeof(header), 1, file.get()) != 1) {
        return make_error("fjterrain: failed writing header to " + path);
    }

    if (header.height_format == FjTerrainHeightFormat::unorm16) {
        std::vector<uint16_t> raw(samples);
        for (size_t i = 0; i < samples; ++i) {
            const float clamped = std::clamp(data.heights[i], 0.0F, 1.0F);
            raw[i] = static_cast<uint16_t>(std::lround(clamped * UNORM16_MAX));
        }
        if (std::fwrite(raw.data(), sizeof(uint16_t), samples, file.get()) != samples) {
            return make_error("fjterrain: failed writing height data to " + path);
        }
    } else {
        if (std::fwrite(data.heights.data(), sizeof(float), samples, file.get()) != samples) {
            return make_error("fjterrain: failed writing height data to " + path);
        }
    }

    if (header.layer_count > 0 && header.weight_count > 0) {
        if (std::fwrite(data.weights.data(), 1, data.weights.size(), file.get()) !=
            data.weights.size()) {
            return make_error("fjterrain: failed writing weight data to " + path);
        }
    }

    if (header.layer_count > 0) {
        if (std::fwrite(data.layers.data(), sizeof(FjTerrainLayer), data.layers.size(),
                        file.get()) != data.layers.size()) {
            return make_error("fjterrain: failed writing layer table to " + path);
        }
        const auto name_count = static_cast<uint32_t>(data.names.size());
        if (std::fwrite(&name_count, sizeof(name_count), 1, file.get()) != 1) {
            return make_error("fjterrain: failed writing name table to " + path);
        }
        for (const auto& name : data.names) {
            const auto length = static_cast<uint32_t>(name.size());
            if (std::fwrite(&length, sizeof(length), 1, file.get()) != 1 ||
                (length > 0 && std::fwrite(name.data(), 1, length, file.get()) != length)) {
                return make_error("fjterrain: failed writing name table to " + path);
            }
        }
    }

    return {};
}

} // namespace fjell
