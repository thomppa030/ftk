#include "ui/nvim_panel.hpp"
#include "core/log.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fjell {

NvimPanel::NvimPanel() = default;

NvimPanel::~NvimPanel() {
    close();
}

void NvimPanel::open(const std::string& file_path, int rows, int cols) {
    // If nvim is already running, switch to the file via :edit instead of restarting
    if (is_open()) {
        // Escape to normal mode, then :edit <path>
        std::string cmd = "\x1b:edit " + file_path + "\n";
        ::write(master_fd_, cmd.data(), cmd.size());
        FJELL_CORE_INFO("Nvim: switching to {}", file_path);
        return;
    }

    rows_ = rows;
    cols_ = cols;
    title_ = "Code Editor";

    // Create libvterm instance
    vt_ = vterm_new(rows_, cols_);
    vterm_set_utf8(vt_, 1);

    // Configure output callback so vterm_keyboard_* writes go to PTY
    vterm_output_set_callback(vt_, [](const char* s, size_t len, void* user) {
        auto* self = static_cast<NvimPanel*>(user);
        if (self->master_fd_ >= 0) {
            ::write(self->master_fd_, s, len);
        }
    }, this);

    screen_ = vterm_obtain_screen(vt_);

    // Enable altscreen (nvim uses it) and reflow on resize
    vterm_screen_enable_altscreen(screen_, 1);
    vterm_screen_enable_reflow(screen_, true);

    // Set default colors to match theme
    VTermColor default_fg{};
    VTermColor default_bg{};
    vterm_color_rgb(&default_fg, 229, 231, 234); // #E5E7EA
    vterm_color_rgb(&default_bg, 0, 0, 0);
    vterm_screen_set_default_colors(screen_, &default_fg, &default_bg);

    // Merge damage at row level for fewer callbacks
    vterm_screen_set_damage_merge(screen_, VTERM_DAMAGE_ROW);

    // Set up screen callbacks
    static VTermScreenCallbacks cbs{};
    cbs.damage = on_damage;
    cbs.movecursor = on_movecursor;
    cbs.settermprop = on_settermprop;
    cbs.sb_pushline = on_sb_pushline;
    cbs.sb_popline = on_sb_popline;
    cbs.sb_clear = on_sb_clear;
    vterm_screen_set_callbacks(screen_, &cbs, this);
    vterm_screen_reset(screen_, 1);

    spawn_nvim(file_path);
    dirty_ = true;
    scrollback_.clear();

    FJELL_CORE_INFO("Opened nvim: {}", file_path);
}

void NvimPanel::close() {
    if (child_pid_ > 0) {
        kill(child_pid_, SIGTERM);
        int status = 0;
        waitpid(child_pid_, &status, WNOHANG);
        child_pid_ = -1;
    }

    if (master_fd_ >= 0) {
        ::close(master_fd_);
        master_fd_ = -1;
    }

    if (vt_) {
        vterm_free(vt_);
        vt_ = nullptr;
        screen_ = nullptr;
    }

    scrollback_.clear();
    prev_grid_cols_ = 0;
    prev_grid_rows_ = 0;
    cursor_visible_ = true;
    cursor_shape_ = VTERM_PROP_CURSORSHAPE_BLOCK;
    mouse_mode_ = VTERM_PROP_MOUSE_NONE;
    title_ = "Code Editor";
}

void NvimPanel::spawn_nvim(const std::string& file_path) {
    struct winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows_);
    ws.ws_col = static_cast<unsigned short>(cols_);

    child_pid_ = forkpty(&master_fd_, nullptr, nullptr, &ws);
    if (child_pid_ < 0) {
        FJELL_CORE_ERROR("forkpty failed: {}", strerror(errno));
        return;
    }

    if (child_pid_ == 0) {
        setenv("TERM", "xterm-256color", 1);
        execlp("nvim", "nvim", file_path.c_str(), nullptr);
        _exit(1);
    }

    int flags = fcntl(master_fd_, F_GETFL, 0);
    fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);
}

void NvimPanel::poll() {
    if (!is_open()) return;

    if (child_pid_ > 0) {
        int status = 0;
        pid_t result = waitpid(child_pid_, &status, WNOHANG);
        if (result > 0) {
            FJELL_CORE_INFO("nvim exited");
            close();
            return;
        }
    }

    char buf[4096];
    while (true) {
        auto n = read(master_fd_, buf, sizeof(buf));
        if (n <= 0) break;
        vterm_input_write(vt_, buf, static_cast<size_t>(n));
    }

    vterm_screen_flush_damage(screen_);
}

void NvimPanel::draw() {
    // Grab mono font (index 2)
    auto& io = ImGui::GetIO();
    if (!mono_font_ && io.Fonts->Fonts.Size > 2) {
        mono_font_ = io.Fonts->Fonts[2];
        cell_width_ = mono_font_->CalcTextSizeA(mono_font_->LegacySize, FLT_MAX, 0.0f, "M").x;
        cell_height_ = mono_font_->LegacySize;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {4.0f, 4.0f});
    if (ImGui::Begin(title_.c_str())) {
        if (!is_open()) {
            ImGui::TextDisabled("Open a script from the Assets panel");
        } else if (!mono_font_) {
            ImGui::TextDisabled("Monospace font not loaded");
        } else {
            // Track focus for focus reporting
            bool now_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            if (now_focused != focused_) {
                focused_ = now_focused;
                if (vt_) {
                    auto* state = vterm_obtain_state(vt_);
                    if (focused_) {
                        vterm_state_focus_in(state);
                    } else {
                        vterm_state_focus_out(state);
                    }
                }
            }

            // Auto-resize terminal to fit window
            auto avail = ImGui::GetContentRegionAvail();
            int new_cols = std::max(10, static_cast<int>(avail.x / cell_width_));
            int new_rows = std::max(4, static_cast<int>(avail.y / cell_height_));

            if (new_cols != prev_grid_cols_ || new_rows != prev_grid_rows_) {
                resize(new_rows, new_cols);
                prev_grid_cols_ = new_cols;
                prev_grid_rows_ = new_rows;
            }

            handle_input();
            render_grid();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

ImU32 NvimPanel::resolve_color(VTermColor color, ImU32 fallback) {
    // Convert any color (indexed, default fg/bg) to RGB
    vterm_screen_convert_color_to_rgb(screen_, &color);
    if (VTERM_COLOR_IS_RGB(&color)) {
        return IM_COL32(color.rgb.red, color.rgb.green, color.rgb.blue, 255);
    }
    return fallback;
}

// UTF-32 codepoint to UTF-8 into a buffer, returns length written
static int utf32_to_utf8(uint32_t cp, char* out) {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

void NvimPanel::render_grid() {
    if (!screen_ || !mono_font_) return;

    ImGui::PushFont(mono_font_);

    auto* draw_list = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float font_size = mono_font_->LegacySize;

    constexpr ImU32 DEFAULT_FG = IM_COL32(229, 231, 234, 255); // #E5E7EA
    constexpr ImU32 DEFAULT_BG = IM_COL32(0, 0, 0, 255);

    for (int row = 0; row < rows_; row++) {
        for (int col = 0; col < cols_; col++) {
            VTermPos pos{row, col};
            VTermScreenCell cell{};
            vterm_screen_get_cell(screen_, pos, &cell);

            float x = origin.x + static_cast<float>(col) * cell_width_;
            float y = origin.y + static_cast<float>(row) * cell_height_;

            // Resolve colors (handles indexed → RGB conversion)
            ImU32 bg = resolve_color(cell.bg, DEFAULT_BG);
            ImU32 fg = resolve_color(cell.fg, DEFAULT_FG);

            // Reverse attribute swaps fg/bg
            if (cell.attrs.reverse) {
                std::swap(fg, bg);
            }

            // Cursor rendering
            bool is_cursor = cursor_visible_
                && row == cursor_pos_.row
                && col == cursor_pos_.col;

            if (is_cursor && cursor_shape_ == VTERM_PROP_CURSORSHAPE_BLOCK) {
                std::swap(fg, bg);
            }

            // Background
            draw_list->AddRectFilled({x, y}, {x + cell_width_, y + cell_height_}, bg);

            // Cursor overlays (drawn after background so they're visible)
            if (is_cursor) {
                if (cursor_shape_ == VTERM_PROP_CURSORSHAPE_UNDERLINE) {
                    draw_list->AddRectFilled(
                        {x, y + cell_height_ - 2.0f},
                        {x + cell_width_, y + cell_height_},
                        DEFAULT_FG);
                } else if (cursor_shape_ == VTERM_PROP_CURSORSHAPE_BAR_LEFT) {
                    draw_list->AddRectFilled(
                        {x, y}, {x + 2.0f, y + cell_height_},
                        DEFAULT_FG);
                }
            }

            // Character
            if (cell.chars[0] != 0 && cell.chars[0] != ' ') {
                char utf8[5]{};
                utf32_to_utf8(cell.chars[0], utf8);

                // Bold — use slightly heavier rendering (offset by 1px)
                if (cell.attrs.bold) {
                    draw_list->AddText(mono_font_, font_size, {x + 1.0f, y}, fg, utf8);
                }
                draw_list->AddText(mono_font_, font_size, {x, y}, fg, utf8);

                // Underline
                if (cell.attrs.underline) {
                    float uy = y + cell_height_ - 2.0f;
                    if (cell.attrs.underline == VTERM_UNDERLINE_DOUBLE) {
                        draw_list->AddLine({x, uy - 2.0f}, {x + cell_width_, uy - 2.0f}, fg);
                    }
                    if (cell.attrs.underline == VTERM_UNDERLINE_CURLY) {
                        // Approximate curly with small segments
                        for (float cx = x; cx < x + cell_width_; cx += 2.0f) {
                            float dy = (static_cast<int>(cx) % 4 < 2) ? -1.0f : 1.0f;
                            draw_list->AddLine({cx, uy + dy}, {cx + 2.0f, uy - dy}, fg);
                        }
                    } else {
                        draw_list->AddLine({x, uy}, {x + cell_width_, uy}, fg);
                    }
                }

                // Strikethrough
                if (cell.attrs.strike) {
                    float sy = y + cell_height_ * 0.5f;
                    draw_list->AddLine({x, sy}, {x + cell_width_, sy}, fg);
                }

                // Italic — draw with slight skew via offset top
                // (true italic would need a separate font; this is a visual hint)
            }
        }
    }

    // Reserve space so ImGui knows the content size
    ImGui::Dummy({static_cast<float>(cols_) * cell_width_,
                  static_cast<float>(rows_) * cell_height_});

    ImGui::PopFont();
}

void NvimPanel::handle_input() {
    if (!focused_) return;

    auto& io = ImGui::GetIO();

    // Build modifier mask
    VTermModifier mod = VTERM_MOD_NONE;
    if (io.KeyShift) mod = static_cast<VTermModifier>(mod | VTERM_MOD_SHIFT);
    if (io.KeyAlt)   mod = static_cast<VTermModifier>(mod | VTERM_MOD_ALT);
    if (io.KeyCtrl)  mod = static_cast<VTermModifier>(mod | VTERM_MOD_CTRL);

    // Text input (printable characters) — use vterm keyboard API
    for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
        uint32_t ch = io.InputQueueCharacters[i];
        if (ch >= 0x20) { // skip control chars, those go through key API
            vterm_keyboard_unichar(vt_, ch, VTERM_MOD_NONE);
        }
    }
    io.InputQueueCharacters.resize(0);

    // Special keys via vterm keyboard API
    struct KeyMap {
        ImGuiKey imgui_key;
        VTermKey vterm_key;
    };

    // clang-format off
    static constexpr KeyMap KEY_MAP[] = {
        {ImGuiKey_Escape,     VTERM_KEY_ESCAPE},
        {ImGuiKey_Enter,      VTERM_KEY_ENTER},
        {ImGuiKey_Backspace,  VTERM_KEY_BACKSPACE},
        {ImGuiKey_Tab,        VTERM_KEY_TAB},
        {ImGuiKey_UpArrow,    VTERM_KEY_UP},
        {ImGuiKey_DownArrow,  VTERM_KEY_DOWN},
        {ImGuiKey_LeftArrow,  VTERM_KEY_LEFT},
        {ImGuiKey_RightArrow, VTERM_KEY_RIGHT},
        {ImGuiKey_Insert,     VTERM_KEY_INS},
        {ImGuiKey_Delete,     VTERM_KEY_DEL},
        {ImGuiKey_Home,       VTERM_KEY_HOME},
        {ImGuiKey_End,        VTERM_KEY_END},
        {ImGuiKey_PageUp,     VTERM_KEY_PAGEUP},
        {ImGuiKey_PageDown,   VTERM_KEY_PAGEDOWN},
    };
    // clang-format on

    for (const auto& [imgui_key, vterm_key] : KEY_MAP) {
        if (ImGui::IsKeyPressed(imgui_key, false)) {
            vterm_keyboard_key(vt_, vterm_key, mod);
        }
    }

    // Function keys F1-F12
    for (int f = 1; f <= 12; f++) {
        auto fkey = static_cast<ImGuiKey>(static_cast<int>(ImGuiKey_F1) + f - 1);
        if (ImGui::IsKeyPressed(fkey, false)) {
            vterm_keyboard_key(vt_, static_cast<VTermKey>(VTERM_KEY_FUNCTION(f)), mod);
        }
    }

    // Ctrl+key combinations (A-Z)
    if (io.KeyCtrl) {
        for (ImGuiKey k = ImGuiKey_A; k <= ImGuiKey_Z;
             k = static_cast<ImGuiKey>(static_cast<int>(k) + 1)) {
            if (ImGui::IsKeyPressed(k, false)) {
                uint32_t ch = 'a' + static_cast<uint32_t>(k - ImGuiKey_A);
                vterm_keyboard_unichar(vt_, ch, VTERM_MOD_CTRL);
            }
        }
    }

    // Mouse input when nvim requests it
    if (mouse_mode_ != VTERM_PROP_MOUSE_NONE) {
        ImVec2 mouse_pos = ImGui::GetMousePos();
        ImVec2 origin = ImGui::GetCursorScreenPos();

        int mcol = static_cast<int>((mouse_pos.x - origin.x) / cell_width_);
        int mrow = static_cast<int>((mouse_pos.y - origin.y) / cell_height_);

        if (mcol >= 0 && mcol < cols_ && mrow >= 0 && mrow < rows_) {
            // Mouse button events
            for (int btn = 0; btn < 3; btn++) {
                if (ImGui::IsMouseClicked(btn)) {
                    vterm_mouse_move(vt_, mrow, mcol, mod);
                    vterm_mouse_button(vt_, btn + 1, true, mod);
                }
                if (ImGui::IsMouseReleased(btn)) {
                    vterm_mouse_move(vt_, mrow, mcol, mod);
                    vterm_mouse_button(vt_, btn + 1, false, mod);
                }
            }

            // Mouse drag / move
            if (mouse_mode_ >= VTERM_PROP_MOUSE_DRAG && ImGui::IsMouseDragging(0)) {
                vterm_mouse_move(vt_, mrow, mcol, mod);
            } else if (mouse_mode_ >= VTERM_PROP_MOUSE_MOVE) {
                vterm_mouse_move(vt_, mrow, mcol, mod);
            }

            // Scroll wheel
            float wheel = io.MouseWheel;
            if (wheel != 0.0f) {
                int button = wheel > 0.0f ? 4 : 5; // 4=up, 5=down
                int clicks = static_cast<int>(std::abs(wheel));
                for (int i = 0; i < clicks; i++) {
                    vterm_mouse_move(vt_, mrow, mcol, mod);
                    vterm_mouse_button(vt_, button, true, mod);
                    vterm_mouse_button(vt_, button, false, mod);
                }
            }
        }
    }
}

void NvimPanel::resize(int rows, int cols) {
    if (rows == rows_ && cols == cols_) return;
    if (rows <= 0 || cols <= 0) return;

    rows_ = rows;
    cols_ = cols;

    if (vt_) {
        vterm_set_size(vt_, rows_, cols_);
    }

    if (master_fd_ >= 0) {
        struct winsize ws{};
        ws.ws_row = static_cast<unsigned short>(rows_);
        ws.ws_col = static_cast<unsigned short>(cols_);
        ioctl(master_fd_, TIOCSWINSZ, &ws);
    }

    dirty_ = true;
}

// --- libvterm callbacks ---

int NvimPanel::on_damage(VTermRect /*rect*/, void* user) {
    static_cast<NvimPanel*>(user)->dirty_ = true;
    return 0;
}

int NvimPanel::on_movecursor(VTermPos pos, VTermPos /*oldpos*/, int /*visible*/,
                             void* user) {
    auto* self = static_cast<NvimPanel*>(user);
    self->cursor_pos_ = pos;
    self->dirty_ = true;
    return 0;
}

int NvimPanel::on_settermprop(VTermProp prop, VTermValue* val, void* user) {
    auto* self = static_cast<NvimPanel*>(user);
    switch (prop) {
        case VTERM_PROP_CURSORVISIBLE:
            self->cursor_visible_ = val->boolean;
            break;
        case VTERM_PROP_CURSORBLINK:
            self->cursor_blink_ = val->boolean;
            break;
        case VTERM_PROP_CURSORSHAPE:
            self->cursor_shape_ = val->number;
            break;
        case VTERM_PROP_TITLE:
            if (val->string.str && val->string.len > 0) {
                if (val->string.initial) {
                    self->title_ = "Code Editor - ";
                }
                self->title_.append(val->string.str, val->string.len);
            }
            break;
        case VTERM_PROP_MOUSE:
            self->mouse_mode_ = val->number;
            break;
        default:
            break;
    }
    return 1;
}

int NvimPanel::on_sb_pushline(int cols, const VTermScreenCell* cells, void* user) {
    auto* self = static_cast<NvimPanel*>(user);
    ScrollbackLine line;
    line.cells.assign(cells, cells + cols);
    self->scrollback_.push_back(std::move(line));

    if (self->scrollback_.size() > MAX_SCROLLBACK) {
        self->scrollback_.erase(self->scrollback_.begin());
    }
    return 1;
}

int NvimPanel::on_sb_popline(int cols, VTermScreenCell* cells, void* user) {
    auto* self = static_cast<NvimPanel*>(user);
    if (self->scrollback_.empty()) return 0;

    auto& line = self->scrollback_.back();
    int copy_cols = std::min(cols, static_cast<int>(line.cells.size()));
    std::memcpy(cells, line.cells.data(),
                static_cast<size_t>(copy_cols) * sizeof(VTermScreenCell));

    // Zero-fill remaining columns
    for (int i = copy_cols; i < cols; i++) {
        std::memset(&cells[i], 0, sizeof(VTermScreenCell));
    }

    self->scrollback_.pop_back();
    return 1;
}

int NvimPanel::on_sb_clear(void* user) {
    static_cast<NvimPanel*>(user)->scrollback_.clear();
    return 1;
}

} // namespace fjell
