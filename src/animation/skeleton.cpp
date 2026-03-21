#include "animation/skeleton.hpp"
#include "animation/fjskel.hpp"
#include "core/log.hpp"

#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>

#include <fstream>
#include <stdexcept>
#include <vector>

namespace fjell {

Skeleton Skeleton::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open skeleton: " + path);
    }

    FjSkelHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (header.magic != FJSKEL_MAGIC) {
        throw std::runtime_error("Invalid .fjskel magic: " + path);
    }
    if (header.version != FJSKEL_VERSION) {
        throw std::runtime_error("Unsupported .fjskel version: " + path);
    }

    const auto num_joints = static_cast<int>(header.num_joints);

    // Read ozz skeleton blob
    std::vector<uint8_t> blob(header.skeleton_size);
    file.seekg(static_cast<std::streamoff>(header.skeleton_offset));
    file.read(reinterpret_cast<char*>(blob.data()),
              static_cast<std::streamsize>(header.skeleton_size));

    // Deserialize via ozz archive
    Skeleton skel;
    ozz::io::MemoryStream stream;
    stream.Write(blob.data(), header.skeleton_size);
    stream.Seek(0, ozz::io::Stream::kSet);

    ozz::io::IArchive archive(&stream);
    archive >> skel.skeleton_;

    // Read inverse bind matrices
    skel.inverse_bind_matrices_.resize(num_joints);
    file.seekg(static_cast<std::streamoff>(header.inv_bind_offset));
    file.read(reinterpret_cast<char*>(skel.inverse_bind_matrices_.data()),
              static_cast<std::streamsize>(num_joints * sizeof(glm::mat4)));

    FJELL_CORE_INFO("Loaded skeleton: {} ({} joints)", path, num_joints);
    return skel;
}

int Skeleton::find_joint(const std::string& name) const {
    const auto& names = skeleton_.joint_names();
    for (int i = 0; i < skeleton_.num_joints(); ++i) {
        if (names[i] == name) {
            return i;
        }
    }
    return -1;
}

} // namespace fjell
