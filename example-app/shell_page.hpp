#pragma once

// The example app's Shell page: the kit's docking on the UI core, as the
// editor lays its panels out (Phase 8 of the UI core plan). A dockspace
// holds a hierarchy, a viewport, an inspector, a console and a history, to
// drag splitters, move tabs along their rows, tear them out to float, and
// dock floating windows again by their title bars. A button builds the
// layout anew.

#include <glm/vec2.hpp>

#include <string>
#include <vector>

namespace ftk::example {

class ShellPage {
public:
    /// The page, its top `top` down the window, under the page tabs.
    void draw(glm::vec2 size, float top);

private:
    bool built_{false};
    int selected_{1};
    std::vector<std::string> lines_{"Loaded scene harbour.fjscene", "Baked 4 light probes in 1.8 s",
                                    "Shader graph rock_wet compiled", "Saved harbour.fjscene"};
};

} // namespace ftk::example
