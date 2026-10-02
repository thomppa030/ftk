#include "ftk/ui/kit/settings_file.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <memory>

namespace ftk::ui {

namespace {

// A section as ImGui's handler holds it: the caller's section, and the
// entry whose lines are being read.
struct Kept {
    SettingsSection section;
    std::string entry;
};

// Every section kept, by name, for the life of the program: ImGui's
// handlers point at them from each context they were added to.
std::vector<std::unique_ptr<Kept>>& kept_sections() {
    static std::vector<std::unique_ptr<Kept>> kept;
    return kept;
}

void* read_open(ImGuiContext*, ImGuiSettingsHandler* handler, const char* name) {
    auto* kept = static_cast<Kept*>(handler->UserData);
    kept->entry = name;
    return kept;
}

void read_line(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    auto* kept = static_cast<Kept*>(entry);
    if (kept->section.read) kept->section.read(kept->entry, line);
}

void write_all(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* out) {
    const auto* kept = static_cast<const Kept*>(handler->UserData);
    if (!kept->section.write) return;
    for (const SettingsEntry& entry : kept->section.write()) {
        out->appendf("[%s][%s]\n", kept->section.name.c_str(), entry.name.c_str());
        for (const std::string& line : entry.lines) {
            out->append(line.c_str());
            out->append("\n");
        }
        out->append("\n");
    }
}

} // namespace

void add_settings_section(SettingsSection section) {
    auto& kept = kept_sections();
    Kept* found = nullptr;
    for (const auto& k : kept) {
        if (k->section.name == section.name) found = k.get();
    }
    if (found == nullptr) {
        found = kept.emplace_back(std::make_unique<Kept>()).get();
        // Set once: ImGui's handlers point at it.
        found->section.name = section.name;
    }
    found->section.read = std::move(section.read);
    found->section.write = std::move(section.write);

    if (ImGui::FindSettingsHandler(found->section.name.c_str()) != nullptr) return;
    ImGuiSettingsHandler handler;
    handler.TypeName = found->section.name.c_str();
    handler.TypeHash = ImHashStr(handler.TypeName);
    handler.ReadOpenFn = read_open;
    handler.ReadLineFn = read_line;
    handler.WriteAllFn = write_all;
    handler.UserData = found;
    ImGui::AddSettingsHandler(&handler);
}

void settings_changed() {
    ImGui::MarkIniSettingsDirty();
}

void save_settings_file(const std::string& path) {
    ImGui::SaveIniSettingsToDisk(path.c_str());
}

void load_settings_file(const std::string& path) {
    ImGui::LoadIniSettingsFromDisk(path.c_str());
}

} // namespace ftk::ui
