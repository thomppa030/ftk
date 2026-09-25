#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace fjell {

class Project;
class ViewportManager;
class VulkanContext;

/// Editor screenshot capture. Two modes:
///   - Scene: temporarily resizes the active editor viewport panel to the
///     requested resolution so the next frame renders at capture size through
///     the normal pipeline, then reads back the post output to PNG.
///   - Pane: copies a rectangle out of the just-presented swapchain image
///     to PNG.
///
/// Lifetime: a request goes Armed → Pending → Idle across two frame phases:
///   Frame N: arm() (Idle → Armed)
///   Frame N+1, after begin_frame, before apply_resize:
///     prepare_for_record() (Armed → Pending) — patches viewport size
///   Frame N+1, after submit_and_present:
///     finish_after_present() (Pending → Idle) — reads back, writes PNG
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
    /// the capture resolution so the upcoming draw_ui's ImGui::Image picks
    /// up the resized post output's descriptor. Drains the GPU first because
    /// PP::resize touches descriptors that may be referenced by in-flight
    /// CBs.
    void resize_for_capture(VulkanContext& vk, ViewportManager& viewports);

    /// Called AFTER draw_ui (still before record_frame). Advances pane-mode
    /// state to capture_ready; scene mode already did its work in
    /// resize_for_capture.
    void prepare_for_record(VulkanContext& vk, ViewportManager& viewports);

    /// Called after submit_and_present. If a capture is pending, reads back
    /// the just-rendered scene (or the swapchain for pane mode) and writes
    /// the PNG. Restores the panel's original size for the next frame.
    void finish_after_present(VulkanContext& vk, ViewportManager& viewports);

private:
    enum class State : std::uint8_t {
        idle,
        armed,         // queued, not yet applied
        warming,       // resize applied, frame N rendering (discarded)
        capture_ready, // frame N+1 rendering — capture this one
    };

    // How many warmup frames to run at capture resolution before grabbing
    // the result. Lets temporal passes (DDGI, exposure, TAA, bloom) settle
    // after the major resize.
    static constexpr int kWarmupFrames = 2;
    int warmup_remaining_{0};

    void do_scene_capture(VulkanContext& vk, ViewportManager& viewports);
    void do_pane_capture(VulkanContext& vk);

    [[nodiscard]] std::filesystem::path next_output_path(const std::string& tag) const;

    const Project* project_{nullptr};
    State state_{State::idle};
    Request request_;

    // What the post pass drew before an ungraded capture, to go back to.
    bool saved_ungraded_{false};

    // Saved editor panel state to restore after capture.
    int saved_panel_id_{0};
    float saved_panel_size_x_{0.0F};
    float saved_panel_size_y_{0.0F};
};

} // namespace fjell
