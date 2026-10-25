#include "ftk/base/file_io.hpp"

#include <format>
#include <fstream>
#include <iterator>

namespace ftk {

Result<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return make_error(std::format("{} can't be opened", path.string()));
    std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (file.bad()) return make_error(std::format("{} can't be read to its end", path.string()));
    return text;
}

} // namespace ftk
