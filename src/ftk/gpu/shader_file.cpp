#include "ftk/gpu/shader.hpp"

#include <fstream>

namespace fjell::gpu {

Result<std::vector<uint32_t>> read_spirv(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return make_error("Shader not found: " + path);
    const auto bytes = static_cast<size_t>(file.tellg());
    if (bytes == 0 || bytes % sizeof(uint32_t) != 0) return make_error("Not SPIR-V: " + path);
    std::vector<uint32_t> words(bytes / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(bytes));
    if (!file) return make_error("Could not read " + path);
    return words;
}

} // namespace fjell::gpu
