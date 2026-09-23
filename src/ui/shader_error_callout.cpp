#include "ui/shader_error_callout.hpp"

#include "core/base_path.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/shader_diagnostic.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/icons.hpp"

#include <imgui.h>

#include <filesystem>
#include <string>

namespace fjell {

bool shader_error_callout(const PipelineCache& cache, uint32_t pipeline_id, const Project* project) {
    if (pipeline_id >= cache.shader_count() || !cache.has_compile_error(pipeline_id)) return false;

    const std::string& path = cache.shader_path(pipeline_id);
    const std::string name = std::filesystem::path(path).filename().string();
    const ShaderDiagnostic error = first_shader_error(cache.compile_error(pipeline_id));

    std::string text;
    if (error.line > 0) {
        text = error.file == "<generated>" ? "In the code generated around it, line "
                                           : "Line ";
        text += std::to_string(error.line) + ": ";
    }
    text += error.message;
    if (!text.empty() && text.back() != '.') text += '.';
    if (error.count > 1) {
        text += " " + std::to_string(error.count - 1) + (error.count == 2 ? " more error." : " more errors.");
    }
    // A shader that compiled once keeps drawing with that version.
    text += cache.generation(pipeline_id) > 0 ? " It draws with the last version that compiled."
                                              : " Nothing has compiled from it yet.";

    const std::string title = name + " failed to compile";
    const std::string action = "Open " + name;
    ImGui::PushID(static_cast<int>(pipeline_id));
    if (ui::callout(ui::Severity::Error, title.c_str(), text.c_str(), ui::icon::shader, action.c_str())) {
        if (auto opened = platform::open_path(base_path::resolve_shared(path, project)); !opened) {
            FJELL_CORE_ERROR("Couldn't open {}: {}", path, opened.error());
        }
    }
    ImGui::PopID();
    return true;
}

} // namespace fjell
