#pragma once

// A window of settings split into pages (sheet 13): floating, not
// dockable, the pages listed down the left and one shown on the right
// under its title.
//
//     if (auto window = ui::PagedWindow("Project settings###settings", &open_)) {
//         if (auto list = ui::PageList("##pages")) {
//             for (Page& page : pages) {
//                 if (ui::page_entry(page.icon, page.name, page.id == current)) current = page.id;
//             }
//         }
//         if (auto body = ui::PageBody("##page")) {
//             ui::page_title(page.icon, page.name);
//             page.draw();
//         }
//     }

#include <imgui.h>

namespace fjell::ui {

/// The window, for as long as it lives: floating over the editor and never
/// docked, first shown centred at `size`, then where it was left. Its
/// title bar closes it through `open`.
class PagedWindow {
public:
    PagedWindow(const char* title, bool* open, ImVec2 size);
    ~PagedWindow();

    PagedWindow(const PagedWindow&) = delete;
    PagedWindow& operator=(const PagedWindow&) = delete;
    PagedWindow(PagedWindow&&) = delete;
    PagedWindow& operator=(PagedWindow&&) = delete;

    explicit operator bool() const { return open_; }

private:
    bool open_{false};
};

/// The page shown, right of the list, padded and scrolling on its own, for
/// as long as it lives. Leaves `reserve_bottom` under it for a footer.
class PageBody {
public:
    explicit PageBody(const char* id, float reserve_bottom = 0.0f);
    ~PageBody();

    PageBody(const PageBody&) = delete;
    PageBody& operator=(const PageBody&) = delete;
    PageBody(PageBody&&) = delete;
    PageBody& operator=(PageBody&&) = delete;

    explicit operator bool() const { return open_; }

private:
    bool open_{false};
};

/// The column of pages, on the sunken surface, for as long as it lives.
/// Takes the window's full height (less `reserve_bottom`, for a footer).
class PageList {
public:
    explicit PageList(const char* id, float reserve_bottom = 0.0f);
    ~PageList();

    PageList(const PageList&) = delete;
    PageList& operator=(const PageList&) = delete;
    PageList(PageList&&) = delete;
    PageList& operator=(PageList&&) = delete;

    explicit operator bool() const { return open_; }

private:
    bool open_{false};
};

/// The width of the page list.
inline constexpr float PAGE_LIST_WIDTH = 172.0f;

/// One page in the list: its icon and name, in the selection wash with the
/// icon in the accent while `selected`. `dimmed` greys it out (a search
/// that found nothing on it). Returns true when clicked.
bool page_entry(const char* icon, const char* label, bool selected, bool dimmed = false);

/// The title over the page shown: its icon in the accent and its name,
/// a step larger than the body text.
void page_title(const char* icon, const char* label);

} // namespace fjell::ui
