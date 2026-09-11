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
    if (data.header.version != FJTERRAIN_VERSION) {
        return make_error("fjterrain: unsupported version " +
                          std::to_string(data.header.version) + " in " + path);
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

        data.layers.resize(data.header.layer_count);
        if (std::fread(data.layers.data(), sizeof(FjTerrainLayer), data.layers.size(),
                       file.get()) != data.layers.size()) {
            return make_error("fjterrain: truncated layer table in " + path);
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
    if (!patches_tile_evenly(data.header)) {
        return make_error("fjterrain: sample_count is not one greater than a multiple "
                          "of patch_quads");
    }

    FilePtr file{std::fopen(path.c_str(), "wb")};
    if (!file) {
        return make_error("fjterrain: cannot write " + path);
    }

    FjTerrainHeader header = data.header;
    header.magic = FJTERRAIN_MAGIC;
    header.version = FJTERRAIN_VERSION;
    header.layer_count = static_cast<uint32_t>(data.layers.size());
    if (data.weights.empty()) {
        header.weight_count = 0;
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
        if (std::fwrite(data.layers.data(), sizeof(FjTerrainLayer), data.layers.size(),
                        file.get()) != data.layers.size()) {
            return make_error("fjterrain: failed writing layer table to " + path);
        }
    }

    return {};
}

} // namespace fjell
