#include "import/source_inspection.hpp"

#include "ftk/base/log.hpp"
#include "import/import_job.hpp"

#include <nlohmann/json.hpp>

namespace fjell {

std::optional<SourceSummary> parse_source_summary(std::string_view line) {
    if (line.empty() || line.front() != '{') return std::nullopt;
    const auto j = nlohmann::json::parse(line, nullptr, false);
    if (!j.is_object() || !j.contains("meshes")) return std::nullopt;
    SourceSummary s;
    s.meshes = j.value("meshes", 0u);
    s.triangles = j.value("triangles", uint64_t{0});
    s.vertices = j.value("vertices", uint64_t{0});
    s.materials = j.value("materials", 0u);
    s.embedded_textures = j.value("embedded_textures", 0u);
    s.texture_files = j.value("texture_files", 0u);
    s.bones = j.value("bones", 0u);
    s.animations = j.value("animations", 0u);
    return s;
}

SourceInspection::~SourceInspection() {
    stop();
}

void SourceInspection::stop() {
    if (state_ == State::reading) platform::close_subprocess(process_);
}

void SourceInspection::start(const std::filesystem::path& source) {
    stop();
    state_ = State::failed;
    buffer_.clear();
    const auto fjimport = ImportJob::find_fjimport();
    if (fjimport.empty()) {
        FJELL_CORE_ERROR("Could not find fjimport to read {}", source.string());
        return;
    }
    auto process = platform::spawn_with_pipe({fjimport.string(), source.string(), "--inspect"});
    if (!process) {
        FJELL_CORE_ERROR("Failed to start fjimport --inspect: {}", process.error());
        return;
    }
    process_ = *process;
    state_ = State::reading;
}

void SourceInspection::poll() {
    if (state_ != State::reading) return;
    const auto take = [&](const std::vector<std::string>& lines) {
        for (const auto& line : lines) {
            if (auto summary = parse_source_summary(line)) {
                summary_ = *summary;
                state_ = State::read;
            }
        }
    };
    take(platform::read_lines(process_, buffer_));
    if (state_ == State::reading && platform::poll_exit(process_)) {
        take(platform::read_lines(process_, buffer_));
        if (state_ == State::reading) state_ = State::failed;
    }
    if (state_ != State::reading) platform::close_subprocess(process_);
}

} // namespace fjell
