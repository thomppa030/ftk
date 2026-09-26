# fjterraingen — procedural terrain generator (.fjterrain)
# Offline tool. NOT linked into the engine runtime.

add_executable(fjterraingen
    main.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/fjterrain_io.cpp
)

target_include_directories(fjterraingen PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(fjterraingen PRIVATE
    glm::glm
)
fjell_target_defaults(fjterraingen)

install(TARGETS fjterraingen RUNTIME DESTINATION .)
