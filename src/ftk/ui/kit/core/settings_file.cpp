#include "ftk/ui/kit/settings_file.hpp"

#include "ftk/base/log.hpp"
#include "ftk/ui/kit/core/kit.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>

// The settings file as ImGui wrote it, so a file saved while ImGui drew the
// kit still reads: entries headed [section][entry], their lines under
// them. The kit's windows that float are kept as [Window][name] with where
// they were left; the docks' trees are not kept here yet (named layouts are
// the shell's, Phase 8 of the UI core plan).

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr std::string_view WINDOWS = "Window";

// Every section kept, for the life of the program, as ImGui keeps its
// handlers.
std::vector<SettingsSection>& sections() {
    static std::vector<SettingsSection> kept;
    return kept;
}

// The program's own settings file, and when something in it first changed
// since it was last written.
struct Kept {
    std::string path{};
    std::optional<double> changed{};
};

Kept& kept_file() {
    static Kept kept;
    return kept;
}

// How long a change waits to be written, as ImGui waits.
constexpr double SAVE_AFTER = 5.0;

// "x,y" as two numbers.
std::optional<glm::vec2> pair_of(std::string_view text) {
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos) return std::nullopt;
    glm::vec2 pair{0.0f};
    const auto x = std::from_chars(text.data(), text.data() + comma, pair.x);
    const auto y = std::from_chars(text.data() + comma + 1, text.data() + text.size(), pair.y);
    if (x.ec != std::errc{} || y.ec != std::errc{}) return std::nullopt;
    return pair;
}

// The windows that float and are remembered, by name, each where it is.
void write_windows(std::string& out) {
    std::vector<const Window*> kept;
    for (const auto& [id, window] : kit().windows) {
        if (window.frame != 0 && window.options.remembered && window.dock == 0) kept.push_back(&window);
    }
    std::ranges::sort(kept, {}, &Window::name);
    for (const Window* window : kept) {
        const glm::vec2 pos = window->rect.min;
        const glm::vec2 size = window->rect.size();
        out += std::format("[{}][{}]\nPos={},{}\nSize={},{}\n\n", WINDOWS, window->name, std::lround(pos.x),
                           std::lround(pos.y), std::lround(size.x), std::lround(size.y));
    }
}

// A line of the window `name`'s entry: it goes where the file had it, now
// or when it is first drawn.
void read_window(std::string_view name, std::string_view line) {
    Window& window = kit().windows[core::hash_label(name, 0)];
    if (window.name.empty()) window.name = name;
    if (line.starts_with("Pos=")) {
        if (const auto pos = pair_of(line.substr(4))) {
            window.rect = {.min = *pos, .max = *pos + window.rect.size()};
            window.placed = true;
        }
    } else if (line.starts_with("Size=")) {
        if (const auto size = pair_of(line.substr(5))) {
            window.rect.max = window.rect.min + *size;
            window.sized = true;
            window.placed = true;
        }
    }
}

} // namespace

void add_settings_section(SettingsSection section) {
    auto& kept = sections();
    const auto found = std::ranges::find(kept, section.name, &SettingsSection::name);
    if (found != kept.end()) {
        *found = std::move(section);
    } else {
        kept.push_back(std::move(section));
    }
}

void settings_changed() {
    Kept& kept = kept_file();
    if (!kept.path.empty() && !kept.changed) kept.changed = ui().time();
}

void keep_settings_file(const std::string& path) {
    kept_file() = {.path = path};
    if (std::filesystem::exists(path)) load_settings_file(path);
}

void save_settings_when_due() {
    Kept& kept = kept_file();
    if (!kept.changed || ui().time() < *kept.changed + SAVE_AFTER) return;
    kept.changed.reset();
    save_settings_file(kept.path);
}

void save_settings_file(const std::string& path) {
    std::string out;
    write_windows(out);
    for (const SettingsSection& section : sections()) {
        if (!section.write) continue;
        for (const SettingsEntry& entry : section.write()) {
            out += std::format("[{}][{}]\n", section.name, entry.name);
            for (const std::string& line : entry.lines) out += line + "\n";
            out += "\n";
        }
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << out;
    if (!file) FTK_CORE_WARN("UI: the settings file {} could not be written", path);
}

void load_settings_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        FTK_CORE_WARN("UI: the settings file {} could not be read", path);
        return;
    }
    std::string type;
    std::string entry;
    for (std::string read; std::getline(file, read);) {
        std::string_view line = read;
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty()) continue;
        // [type][entry] begins an entry; the name runs to the last ].
        if (line.front() == '[' && line.back() == ']') {
            const std::size_t close = line.find(']');
            type = line.substr(1, close - 1);
            entry = close + 2 < line.size() && line[close + 1] == '['
                        ? std::string(line.substr(close + 2, line.size() - close - 3))
                        : std::string();
            continue;
        }
        if (type == WINDOWS) {
            read_window(entry, line);
            continue;
        }
        // Found again at every line: a section's reading may add another.
        const auto& kept = sections();
        const auto section = std::ranges::find(kept, type, &SettingsSection::name);
        if (section != kept.end() && section->read) section->read(entry, line);
    }
}

} // namespace ftk::ui
