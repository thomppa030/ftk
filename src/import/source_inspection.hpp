#pragma once

#include "ftk/platform/platform.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace fjell {

/// What a model file holds, read before it is imported.
struct SourceSummary {
    uint32_t meshes{0};
    uint64_t triangles{0};
    uint64_t vertices{0};
    uint32_t materials{0};
    uint32_t embedded_textures{0};
    uint32_t texture_files{0};   // textures its materials name as files beside it
    uint32_t bones{0};           // a skinned model's skeleton
    uint32_t animations{0};
};

/// The summary fjimport --inspect prints as one JSON line; none when the
/// line isn't one.
[[nodiscard]] std::optional<SourceSummary> parse_source_summary(std::string_view line);

/// Reads a model file's summary through fjimport --inspect, in the
/// background, so a large file doesn't hold the dialog up.
class SourceInspection {
public:
    enum class State { idle, reading, read, failed };

    SourceInspection() = default;
    ~SourceInspection();
    SourceInspection(const SourceInspection&) = delete;
    SourceInspection& operator=(const SourceInspection&) = delete;

    /// Starts reading `source`, dropping any read still going.
    void start(const std::filesystem::path& source);
    /// Picks up what fjimport has printed. Call once a frame.
    void poll();

    [[nodiscard]] State state() const { return state_; }
    /// Valid once state() is read.
    [[nodiscard]] const SourceSummary& summary() const { return summary_; }

private:
    void stop();

    State state_{State::idle};
    SourceSummary summary_;
    ftk::platform::SubprocessHandle process_;
    std::string buffer_;
};

} // namespace fjell
