#include "core/engine.hpp"
#include "core/log.hpp"

#include <cstdlib>

int main() {
    fjell::log::init();

    try {
        fjell::Engine engine{"Fjell Engine", 1280, 720};
        engine.run();
    } catch (const std::exception& e) {
        FJELL_CORE_CRITICAL("Fatal: {}", e.what());
        fjell::log::shutdown();
        return EXIT_FAILURE;
    }

    fjell::log::shutdown();
    return EXIT_SUCCESS;
}
