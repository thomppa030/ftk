#pragma once

#include "environment/day_profile_asset.hpp"
#include "ui/track_asset_editor_context.hpp"

#include <string>

namespace fjell {

/// Edits a `.fjday` day profile: the tracks that shape the sun, ambient,
/// sky, fog and exposure over 24 hours. The timeline scrubs the open scene's
/// hour and the preview renders the scene under the profile at that hour,
/// so the look is shaped where it is seen.
class DayProfileEditorContext final : public TrackAssetEditorContext {
public:
    explicit DayProfileEditorContext(TrackAssetEditorDeps deps);

    [[nodiscard]] const char* context_type() const override { return "day_profile"; }
    [[nodiscard]] std::string document() const override;
    [[nodiscard]] Result<> write() override;
    [[nodiscard]] Result<> restore(const std::string& text) override;
    void draw_header_actions() override;

private:
    [[nodiscard]] bool load() override;
    void sync_timeline() override;
    void scrubbed(float t) override;
    void draw_properties() override;
    void push_live() override;

    [[nodiscard]] float scene_hour() const;

    DayProfileAsset profile_;
    float cursor_hour_{12.0f};
};

} // namespace fjell
