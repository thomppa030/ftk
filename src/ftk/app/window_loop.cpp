#include "ftk/app/window_loop.hpp"

namespace ftk {

Result<> run_window_loop(const WindowLoopSteps& steps) {
    for (;;) {
        steps.take_events();
        if (steps.update) steps.update();
        if (steps.closing()) return {};
        if (Result<> drawn = steps.draw(); !drawn) return drawn;
        steps.wait();
    }
}

} // namespace ftk
