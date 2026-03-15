#include "core/engine.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    try {
        fjell::Engine engine{"Fjell Engine", 1280, 720};
        engine.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
