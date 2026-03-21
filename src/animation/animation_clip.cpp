#include "animation/animation_clip.hpp"
#include "animation/fjanim.hpp"
#include "core/log.hpp"

#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>

#include <fstream>
#include <stdexcept>
#include <vector>

namespace fjell {

AnimationClip AnimationClip::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open animation: " + path);
    }

    FjAnimHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (header.magic != FJANIM_MAGIC) {
        throw std::runtime_error("Invalid .fjanim magic: " + path);
    }
    if (header.version != FJANIM_VERSION) {
        throw std::runtime_error("Unsupported .fjanim version: " + path);
    }

    // Read clip name
    AnimationClip clip;
    clip.name_.resize(header.name_length);
    file.seekg(static_cast<std::streamoff>(header.name_offset));
    file.read(clip.name_.data(), static_cast<std::streamsize>(header.name_length));

    // Read ozz animation blob
    std::vector<uint8_t> blob(header.data_size);
    file.seekg(static_cast<std::streamoff>(header.data_offset));
    file.read(reinterpret_cast<char*>(blob.data()),
              static_cast<std::streamsize>(header.data_size));

    // Deserialize via ozz archive
    ozz::io::MemoryStream stream;
    stream.Write(blob.data(), header.data_size);
    stream.Seek(0, ozz::io::Stream::kSet);

    ozz::io::IArchive archive(&stream);
    archive >> clip.animation_;

    FJELL_CORE_INFO("Loaded animation clip: {} ({:.2f}s, {} tracks)",
                    clip.name_, clip.animation_.duration(), clip.animation_.num_tracks());
    return clip;
}

} // namespace fjell
