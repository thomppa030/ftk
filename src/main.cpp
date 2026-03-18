#include "core/engine.hpp"
#include "core/log.hpp"

#include <cstdlib>
#include <filesystem>

int main(int argc, char* argv[]) {
    fjell::log::init();

    std::filesystem::path project_path;
    if (argc > 1) {
        project_path = argv[1];
    }

    try {
        fjell::Engine engine{"Fjell Engine", 2560, 1440, project_path};
        engine.run();
    } catch (const std::exception& e) {
        FJELL_CORE_CRITICAL("Fatal: {}", e.what());
        fjell::log::shutdown();
        return EXIT_FAILURE;
    }

    fjell::log::shutdown();
    return EXIT_SUCCESS;
}
