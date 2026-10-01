#pragma once

#include "ftk/gpu/readback.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace fjell {

class Project;
class ViewportManager;
class VulkanContext;

/// Editor screenshot capture. Two modes:
///   - Scene: temporarily resizes the active editor viewport panel to the
///     requested resolution so the frames render at capture size through
///     the normal pipeline, then reads back the post output to PNG.
///   - Pane: reads a rectangle of the frame's own swapchain image, once the
///     editor's UI is drawn into it and before it is presented, to PNG.
///
/// A request goes armed → (scene: warming for a few frames) → capture_ready
/// → reading → idle. record_read() records the read into the frame that
/// renders the capture, after its views and UI; finish_after_present() on a
/// later frame finds it done and writes the PNG.
class CaptureService {
public:
    enum class Mode : std::uint8_t {
        scene,
        pane,
    };

    struct Request {
        Mode mode{Mode::scene};
        uint32_t width{1920};
        uint32_t height{1080};
        bool include_overlays{false};   // scene mode: keep grid/gizmos
        /// Scene mode: the engine's default look, whatever post process
        /// volumes the camera is in, for a frame to grade elsewhere.
        bool ungraded{false};
        std::string pane_name;
    };

    void set_project(const Project* p) { project_ = p; }

    /// Queue a capture for the next frame.
    void request(const Request& r);
    [[nodiscard]] bool has_pending() const { return state_ != State::idle; }

    /// Called right after apply_resize, BEFORE draw_ui. If a scene capture
    /// is armed/warming, resizes the editor viewport's offscreen + PP to
    /// the capture resolution, so the upcoming draw_ui shows the resized
    /// post output.
    void resize_for_capture(VulkanContext& vk, ViewportManager& viewports);

    /// Called AFTER draw_ui (still before record_frame). Advances pane-mode
    /// state to capture_ready; scene mode already did its work in
    /// resize_for_capture.
    void prepare_for_record(VulkanContext& vk, ViewportManager& viewports);

    /// Called after record_frame, before the frame is sent. A capture ready
    /// to be taken records its read into the frame: the scene's post output,
    /// or the pane's rectangle of the swapchain image. The panel goes back
    /// to its own size for the next frame.
    void record_read(VulkanContext& vk, ViewportManager& viewports);

    /// Called after begin_frame. Counts down a scene capture's warmup, and
    /// writes the PNG of a read whose frame has finished.
    void finish_after_present(VulkanContext& vk, ViewportManager& viewports);

private:
    enum class State : std::uint8_t {
        idle,
        armed,         // queued, not yet applied
        warming,       // resize applied, frame N rendering (discarded)
        capture_ready, // frame N+1 rendering — capture this one
        reading,       // its read on the way back
    };

    // How many warmup frames to run at capture resolution before grabbing
    // the result. Lets temporal passes (DDGI, exposure, TAA, bloom) settle
    // after the major resize.
    static constexpr int kWarmupFrames = 2;
    int warmup_remaining_{0};

    void read_scene(VulkanContext& vk, ViewportManager& viewports);
    void read_pane(VulkanContext& vk);

    [[nodiscard]] std::filesystem::path next_output_path(const std::string& tag) const;

    const Project* project_{nullptr};
    State state_{State::idle};
    Request request_;

    // What the post pass drew before an ungraded capture, to go back to.
    bool saved_ungraded_{false};

    // The capture's read, and what its PNG is named after.
    std::optional<ftk::gpu::Readback> read_;
    std::string read_tag_;
    // Saved editor panel state to restore after capture.
    int saved_panel_id_{0};
    float saved_panel_size_x_{0.0F};
    float saved_panel_size_y_{0.0F};
};

} // namespace fjell
