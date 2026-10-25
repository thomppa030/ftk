#include "ftk/app/desktop_entry.hpp"
#include "ftk/base/file_io.hpp"
#include "ftk/image/png.hpp"
#include "ftk/image/svg.hpp"

#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <string_view>
#include <system_error>

namespace ftk {

namespace fs = std::filesystem;

namespace {

// The characters an Exec argument is quoted for (the Desktop Entry
// Specification, "The Exec key").
constexpr std::string_view RESERVED = " \t\n\"'\\><~|&;$*?#()`";

// `value` as a desktop entry's string writes it: a backslash, a newline, a
// tab and a carriage return escaped.
std::string desktop_string(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

// A desktop entry's string as it reads: its escapes undone.
std::string read_desktop_string(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 == value.size()) {
            out += value[i];
            continue;
        }
        switch (value[++i]) {
            case 's': out += ' '; break;
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '\\': out += '\\'; break;
            default:
                out += '\\';
                out += value[i];
        }
    }
    return out;
}

// The first argument of an Exec line, its string escapes already undone: a
// quoted one up to its closing quote with its own escapes undone, else up to
// the first space; "%%" read as "%".
std::string first_argument(std::string_view exec) {
    std::string argument;
    if (!exec.empty() && exec.front() == '"') {
        for (std::size_t i = 1; i < exec.size() && exec[i] != '"'; ++i) {
            if (exec[i] == '\\' && i + 1 < exec.size()) ++i;
            argument += exec[i];
        }
    } else {
        argument = exec.substr(0, exec.find(' '));
    }
    std::string unpercented;
    for (std::size_t i = 0; i < argument.size(); ++i) {
        unpercented += argument[i];
        if (argument[i] == '%' && i + 1 < argument.size() && argument[i + 1] == '%') ++i;
    }
    return unpercented;
}

Result<> write_file(const fs::path& path, std::string_view bytes) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return make_error(std::format("{} can't be made: {}", path.parent_path().string(), error.message()));
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) return make_error(std::format("{} can't be written", path.string()));
    return {};
}

} // namespace

std::string desktop_exec(const fs::path& program) {
    const std::string path = program.string();
    std::string argument;
    if (path.find_first_of(RESERVED) == std::string::npos) {
        argument = path;
    } else {
        argument = "\"";
        for (const char c : path) {
            if (c == '"' || c == '`' || c == '$' || c == '\\') argument += '\\';
            argument += c;
        }
        argument += '"';
    }
    std::string percented;
    for (const char c : argument) {
        percented += c;
        if (c == '%') percented += '%';
    }
    return desktop_string(percented);
}

fs::path desktop_data_home() {
    if constexpr (!HAS_DESKTOP_ENTRIES) return {};
    if (const char* data = std::getenv("XDG_DATA_HOME"); data != nullptr && fs::path(data).is_absolute()) return data;
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return fs::path(home) / ".local" / "share";
    }
    return {};
}

std::optional<fs::path> desktop_entry_program(std::string_view id, const fs::path& data_home) {
    const auto text = read_text(data_home / "applications" / (std::string(id) + ".desktop"));
    if (!text) return std::nullopt;
    bool in_entry = false;
    std::string_view rest = *text;
    while (!rest.empty()) {
        const std::size_t end = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.starts_with('[')) {
            in_entry = line == "[Desktop Entry]";
        } else if (in_entry && line.starts_with("Exec=")) {
            const std::string program = first_argument(read_desktop_string(line.substr(5)));
            if (program.empty()) return std::nullopt;
            return fs::path(program);
        }
    }
    return std::nullopt;
}

Result<fs::path> create_desktop_entry(const DesktopEntry& entry, const fs::path& data_home) {
    if (data_home.empty()) return make_error("There is no folder for desktop entries");
    if (entry.id.empty() || entry.id.find_first_of("/\\") != std::string::npos) {
        return make_error(std::format("\"{}\" can't name a desktop entry", entry.id));
    }
    const auto svg = read_text(entry.icon);
    if (!svg) return make_error(std::format("The icon: {}", svg.error()));
    const auto own = image::svg_size(*svg);
    if (!own) return make_error(std::format("The icon {}: {}", entry.icon.string(), own.error()));
    if (std::abs(own->width - own->height) > 0.01f * own->height) {
        return make_error(std::format("The icon {} isn't square ({} × {})", entry.icon.string(), own->width, own->height));
    }

    const fs::path icons = data_home / "icons" / "hicolor";
    if (auto written = write_file(icons / "scalable" / "apps" / (entry.id + ".svg"), *svg); !written) {
        return std::unexpected(written.error());
    }
    for (const uint32_t size : DESKTOP_ICON_SIZES) {
        const auto drawn = image::rasterize_svg(*svg, size, size);
        if (!drawn) return make_error(std::format("The icon at {} pixels: {}", size, drawn.error()));
        const std::vector<std::byte> png = image::encode_png(drawn->texels, drawn->width, drawn->height, 4);
        if (png.empty()) return make_error(std::format("The icon at {} pixels can't be encoded", size));
        const std::string folder = std::format("{}x{}", size, size);
        const std::string_view bytes{reinterpret_cast<const char*>(png.data()), png.size()};
        if (auto written = write_file(icons / folder / "apps" / (entry.id + ".png"), bytes); !written) {
            return std::unexpected(written.error());
        }
    }

    std::string categories;
    for (const std::string& category : entry.categories) categories += desktop_string(category) + ';';
    const std::string text = std::format("[Desktop Entry]\n"
                                         "Type=Application\n"
                                         "Name={}\n"
                                         "Comment={}\n"
                                         "Exec={}\n"
                                         "TryExec={}\n"
                                         "Icon={}\n"
                                         "Terminal=false\n"
                                         "Categories={}\n"
                                         "StartupWMClass={}\n",
                                         desktop_string(entry.name), desktop_string(entry.comment),
                                         desktop_exec(entry.program), desktop_string(entry.program.string()), entry.id,
                                         categories, entry.id);
    const fs::path path = data_home / "applications" / (entry.id + ".desktop");
    if (auto written = write_file(path, text); !written) return std::unexpected(written.error());
    return path;
}

} // namespace ftk
