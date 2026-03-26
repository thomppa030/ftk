# --- Testable engine subset (no Vulkan, no GLFW, no ImGui) ---
add_library(fjell-testable STATIC
    test_log_stub.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjui_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjss_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/fjsl_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/material_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/style_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_data_bind.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_animation.cpp
)

target_include_directories(fjell-testable PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fjell-testable PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(fjell-testable PRIVATE /W4 /WX)
else()
    target_compile_options(fjell-testable PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# --- Test executable ---
add_executable(fjell-tests
    test_resource_handle.cpp
    test_delegate.cpp
    test_fjui_parser.cpp
    test_fjss_parser.cpp
    test_fjsl_parser.cpp
    test_style_resolver.cpp
    test_data_bind.cpp
    test_ui_animation.cpp
)

target_link_libraries(fjell-tests PRIVATE
    fjell-testable
    Catch2::Catch2WithMain
)

if(MSVC)
    target_compile_options(fjell-tests PRIVATE /W4 /WX)
else()
    target_compile_options(fjell-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(fjell-tests)
