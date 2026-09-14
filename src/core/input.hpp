#ifndef FJELL_INPUT_HPP
#define FJELL_INPUT_HPP

#include "fjell/api.h"
#include "core/delegate.hpp"
#include <glm/glm.hpp>

#include <array>
#include <string>

struct GLFWwindow;

namespace fjell {

/// Which kind of device the player is currently using. Drives prompts and
/// anything else that should follow the player between keyboard and pad; it
/// does not gate input itself, since every bound source keeps being read.
enum class InputDevice : uint8_t { keyboard_mouse, gamepad };

[[nodiscard]] constexpr const char* input_device_name(InputDevice device) {
    return device == InputDevice::gamepad ? "gamepad" : "keyboard_mouse";
}

class Input {
public:
    void attach(GLFWwindow* window);
    void update();

    [[nodiscard]] GLFWwindow* window() const { return window_; }

    /// These read idle/zero while docked play mode has focus and the game
    /// hasn't been given it (see set_game_focus) — a player's script and the
    /// editor's own camera/gizmo code share this same Input, and without the
    /// gate a click meant to move the player also orbits the viewport camera
    /// and vice versa. Editor code that must keep working regardless of
    /// focus (the viewport's own camera controls, its navigation-mode check)
    /// reads through the raw_* variants below instead.
    [[nodiscard]] FJELL_SDK_EXPORT bool is_key_pressed(int key) const;
    [[nodiscard]] bool is_key_held(int key) const;
    [[nodiscard]] bool is_key_released(int key) const;

    [[nodiscard]] bool is_mouse_button_pressed(int button) const;
    [[nodiscard]] bool is_mouse_button_held(int button) const;

    [[nodiscard]] glm::dvec2 mouse_position() const { return mouse_pos_; }
    [[nodiscard]] glm::dvec2 mouse_delta() const;
    [[nodiscard]] glm::dvec2 scroll_delta() const;

    /// Bypasses the game-focus gate above. For editor input consumers only
    /// (the viewport's fly camera, its own navigation-mode check) — never
    /// use these for anything the player's game itself should react to.
    [[nodiscard]] bool raw_key_pressed(int key) const;
    [[nodiscard]] bool raw_key_held(int key) const;
    [[nodiscard]] bool raw_mouse_button_held(int button) const;
    [[nodiscard]] glm::dvec2 raw_mouse_delta() const { return mouse_delta_; }
    [[nodiscard]] glm::dvec2 raw_scroll_delta() const { return scroll_delta_; }

    /// Whether the running game currently owns keyboard/mouse/pad input, as
    /// opposed to the editor around it (docked play only — fullscreen is
    /// always focused). Set once per frame by PlayController; query methods
    /// above read it, so nothing else needs to check it directly.
    void set_game_focus(bool focused) { game_focus_ = focused; }
    [[nodiscard]] bool game_focus() const { return game_focus_; }

    void set_cursor_captured(GLFWwindow* window, bool captured);
    [[nodiscard]] bool is_cursor_captured() const { return cursor_captured_; }

    /// Load SDL gamepad mappings so controllers GLFW does not know are
    /// still usable. Call once before the first update(). Without a mapping
    /// glfwGetGamepadState fails outright and the pad reads as absent.
    static void load_gamepad_mappings();

    // Gamepad
    [[nodiscard]] bool pad_connected() const { return pad_connected_; }
    [[nodiscard]] bool pad_button_held(int button) const;
    [[nodiscard]] bool pad_button_pressed(int button) const;
    [[nodiscard]] float pad_axis(int axis) const;
    /// Name of the pad currently being read, empty when none is connected.
    [[nodiscard]] const std::string& pad_name() const { return pad_name_; }

    /// The device the player last used with enough intent to count. Starts
    /// as keyboard_mouse and only changes when the other device is used
    /// past a threshold — see the switch thresholds in the implementation.
    [[nodiscard]] InputDevice active_device() const { return active_device_; }

    /// Fires when active_device() changes, with the new device.
    Delegate<void(InputDevice)> on_device_changed;

private:
    static constexpr int MAX_KEYS = 512;
    static constexpr int MAX_BUTTONS = 8;
    static constexpr int MAX_PAD_BUTTONS = 15;
    static constexpr int MAX_PAD_AXES = 6;

    enum class KeyState : uint8_t { Idle, Pressed, Held, Released };

    std::array<KeyState, MAX_KEYS> keys_{};
    std::array<KeyState, MAX_BUTTONS> mouse_buttons_{};

    // Gamepad state
    std::array<uint8_t, MAX_PAD_BUTTONS> pad_buttons_{};
    std::array<uint8_t, MAX_PAD_BUTTONS> pad_buttons_prev_{};
    std::array<float, MAX_PAD_AXES> pad_axes_{};
    bool pad_connected_{false};

    /// Which joystick slot is being read, or -1 when none is. Resolved by
    /// scanning rather than fixed to slot 0, so a pad on another port or one
    /// plugged in after startup is still found.
    int pad_jid_{-1};
    std::string pad_name_;

    InputDevice active_device_{InputDevice::keyboard_mouse};

    /// Update active_device_ from this frame's input, firing the delegate on
    /// a change. Thresholds live in the implementation.
    void detect_active_device();
    /// Slots already tried and found unmappable, cleared when they empty, so
    /// the per-frame scan reports each connection once instead of every frame.
    /// Sized for GLFW's joystick slots, which are ids 0 through 15.
    static constexpr int MAX_JOYSTICKS = 16;
    std::array<bool, MAX_JOYSTICKS> reported_unmapped_{};

    /// Pick a connected, mapped joystick to read. Keeps the current one while
    /// it stays valid so a second pad cannot steal the slot mid-session.
    void resolve_pad();

    glm::dvec2 mouse_pos_{0.0};
    glm::dvec2 mouse_last_{0.0};
    glm::dvec2 mouse_delta_{0.0};
    glm::dvec2 scroll_delta_{0.0};
    GLFWwindow* window_{nullptr};
    bool first_mouse_{true};
    bool cursor_captured_{false};

    // true outside docked play (fullscreen play, or not playing at all) so
    // every other caller keeps working unchanged; PlayController is the only
    // writer once docked play makes the distinction meaningful.
    bool game_focus_{true};

    static void key_callback(GLFWwindow* window, int key, int scancode,
                             int action, int mods);
    static void cursor_callback(GLFWwindow* window, double xpos, double ypos);
    static void mouse_button_callback(GLFWwindow* window, int button,
                                      int action, int mods);
    static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
};

} // namespace fjell

#endif // FJELL_INPUT_HPP
