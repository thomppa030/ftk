#pragma once

#include "environment/weather_asset.hpp"
#include "renderer/resources/resource_handle.hpp"
#include "ui/track_asset_editor_context.hpp"

#include <string>

namespace fjell {

/// Edits a `.fjweather` event: its phases and their durations, what falls
/// and how the surfaces take it, the wind, the sky and profile it brings,
/// its effects and sounds, and the tracks keyed over its four phases. Play
/// starts the event over the open scene and the timeline follows it, so the
/// storm is shaped while it happens.
class WeatherEditorContext final : public TrackAssetEditorContext {
public:
    explicit WeatherEditorContext(TrackAssetEditorDeps deps);

    [[nodiscard]] const char* context_type() const override { return "weather"; }
    [[nodiscard]] std::string document() const override;
    [[nodiscard]] ftk::Result<> write() override;
    [[nodiscard]] ftk::Result<> restore(const std::string& text) override;
    void draw_header_actions() override;

private:
    [[nodiscard]] bool load() override;
    [[nodiscard]] TrackSet& tracks() override { return event_.tracks; }
    void sync_timeline() override;
    void scrubbed(float t) override;
    void draw_properties() override;
    void push_live() override;

    /// Start the edited event over the open scene at `phase`.
    void play_from(WeatherPhase phase);
    [[nodiscard]] WeatherAssetHandle live_handle();
    [[nodiscard]] bool is_playing_here() const;

    WeatherAsset event_;
    float cursor_{0.0f};            // phase units, 0..4
};

} // namespace fjell
