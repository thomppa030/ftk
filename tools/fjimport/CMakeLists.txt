# fjimport — offline mesh importer (glTF/GLB/FBX → .fjmesh)
# Links Assimp + meshoptimizer. NOT linked into the engine runtime.

add_executable(fjimport
    main.cpp
    log_stub.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/meshlet.cpp
)

target_include_directories(fjimport PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(fjimport PRIVATE
    assimp::assimp
    meshoptimizer
    glm::glm
    spdlog::spdlog
)

if(MSVC)
    target_compile_options(fjimport PRIVATE /W4 /WX)
else()
    target_compile_options(fjimport PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Treat Assimp headers as system headers to suppress warnings
get_target_property(ASSIMP_INCLUDE_DIRS assimp::assimp INTERFACE_INCLUDE_DIRECTORIES)
if(ASSIMP_INCLUDE_DIRS)
    target_include_directories(fjimport SYSTEM PRIVATE ${ASSIMP_INCLUDE_DIRS})
endif()
