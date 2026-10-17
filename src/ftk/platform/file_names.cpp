#include "ftk/platform/file_names.hpp"

namespace ftk::platform {

std::vector<std::string> free_names(const std::filesystem::path& folder, std::span<const NameParts> names) {
    std::vector<std::string> named(names.size());
    for (int number = 0;; ++number) {
        const std::string suffix = number == 0 ? std::string{} : "_" + std::to_string(number);
        for (std::size_t i = 0; i < names.size(); ++i) named[i] = names[i].stem + suffix + names[i].extension;
        bool free = true;
        for (std::size_t i = 0; i < names.size() && free; ++i) {
            std::error_code ec;
            free = !std::filesystem::exists(folder / names[i].within / named[i], ec);
        }
        if (free) return named;
    }
}

std::string free_name(const std::filesystem::path& folder, const NameParts& name) {
    return free_names(folder, std::span(&name, 1)).front();
}

} // namespace ftk::platform
