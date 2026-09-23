#pragma once

#include <string>

namespace fjell::ui {

/// A window showing every piece of the kit and every theme token, to hold
/// the editor's real look against the approved design. Opened from View.
class KitGallery {
public:
    /// Draws the window; `open` is cleared when it is closed.
    void draw(bool* open);

private:
    void buttons();
    void rows();

    // Values for the sample controls to edit.
    std::string name_{"Crate"};
    float intensity_{3.2f};
    bool shadows_{true};
    bool snap_{true};
    bool grid_{false};
    bool paint_{false};
};

} // namespace fjell::ui
