# fjlint — checks .fjss stylesheets, .fjwidget files, .fjfont recipes and
# .fjsl shaders without the engine.
#
# Links the same parser sources the engine uses. fjsl_parser.cpp lives under
# renderer/ but is pure text processing — no Vulkan, no GPU state — so it
# links here like the rest.

add_executable(fjlint
    main.cpp
    ${CMAKE_SOURCE_DIR}/src/core/log.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/fjsl_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/scripting/reflected_types.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjss_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjss_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjui_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjui_serializer.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_style_properties.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/widget_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/widget_check.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_actions.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_value.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_context.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_pipes.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjfont.cpp
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

fjell_target_defaults(fjlint)
if(MSVC)
    target_compile_definitions(fjlint PRIVATE FJELL_ENGINE_BUILD)
endif()

install(TARGETS fjlint RUNTIME DESTINATION .)
