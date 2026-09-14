# fjlint — checks .fjss stylesheets and .fjwidget files without the engine.
# Links the same parser sources the engine uses; nothing from the renderer.

add_executable(fjlint
    main.cpp
    log_stub.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjss_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjui_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjui_serializer.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_style_properties.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/widget_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_widget_context.cpp
)

target_include_directories(fjlint PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/scripting
)

target_link_libraries(fjlint PRIVATE
    glm::glm
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Vulkan::Headers
)

if(MSVC)
    target_compile_options(fjlint PRIVATE /W4 /WX)
    target_compile_definitions(fjlint PRIVATE FJELL_ENGINE_BUILD)
else()
    target_compile_options(fjlint PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

install(TARGETS fjlint RUNTIME DESTINATION .)
