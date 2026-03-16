#pragma once

#include <imgui.h>
#include <vterm.h>

#include <string>
#include <vector>

#include <sys/types.h>

namespace fjell {

class NvimPanel {
public:
    NvimPanel();
    ~NvimPanel();

    NvimPanel(const NvimPanel&) = delete;
    NvimPanel& operator=(const NvimPanel&) = delete;
    NvimPanel(NvimPanel&&) = delete;
    NvimPanel& operator=(NvimPanel&&) = delete;

    void open(const std::string& file_path, int rows = 40, int cols = 120);
    void close();
    void draw();
    void poll();

    [[nodiscard]] bool is_open() const { return master_fd_ >= 0; }
    [[nodiscard]] const std::string& title() const { return title_; }

private:
    void spawn_nvim(const std::string& file_path);
    void render_grid();
    void handle_input();
    void resize(int rows, int cols);

    ImU32 resolve_color(VTermColor color, ImU32 fallback);

    // libvterm screen callbacks
    static int on_damage(VTermRect rect, void* user);
    static int on_movecursor(VTermPos pos, VTermPos oldpos, int visible, void* user);
    static int on_settermprop(VTermProp prop, VTermValue* val, void* user);
    static int on_sb_pushline(int cols, const VTermScreenCell* cells, void* user);
    static int on_sb_popline(int cols, VTermScreenCell* cells, void* user);
    static int on_sb_clear(void* user);

    // PTY
    int master_fd_{-1};
    pid_t child_pid_{-1};

    // libvterm
    VTerm* vt_{nullptr};
    VTermScreen* screen_{nullptr};
    int rows_{40};
    int cols_{120};

    // Cursor
    VTermPos cursor_pos_{};
    bool cursor_visible_{true};
    int cursor_shape_{VTERM_PROP_CURSORSHAPE_BLOCK};
    bool cursor_blink_{false};

    // Mouse
    int mouse_mode_{VTERM_PROP_MOUSE_NONE};

    // Scrollback
    struct ScrollbackLine {
        std::vector<VTermScreenCell> cells;
    };
    std::vector<ScrollbackLine> scrollback_;
    static constexpr size_t MAX_SCROLLBACK = 1000;

    // State
    std::string title_{"Code Editor"};
    bool dirty_{true};
    bool focused_{false};
    ImFont* mono_font_{nullptr};
    float cell_width_{0.0f};
    float cell_height_{0.0f};
    int prev_grid_cols_{0};
    int prev_grid_rows_{0};
};

} // namespace fjell
