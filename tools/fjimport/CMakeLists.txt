# fjimport — offline mesh importer (glTF/GLB/FBX → .fjmesh)
# Links Assimp + meshoptimizer. NOT linked into the engine runtime.

add_executable(fjimport
    main.cpp
    log_stub.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/meshlet.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/sdf_generator.cpp
)

target_include_directories(fjimport PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(fjimport PRIVATE
    assimp::assimp
    meshoptimizer
    ozz_animation
    ozz_animation_offline
    glm::glm
    nlohmann_json::nlohmann_json
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

# Treat ozz-animation headers as system headers to suppress warnings
get_target_property(OZZ_BASE_DIRS ozz_base INTERFACE_INCLUDE_DIRECTORIES)
if(OZZ_BASE_DIRS)
    target_include_directories(fjimport SYSTEM PRIVATE ${OZZ_BASE_DIRS})
endif()
get_target_property(OZZ_ANIM_DIRS ozz_animation INTERFACE_INCLUDE_DIRECTORIES)
if(OZZ_ANIM_DIRS)
    target_include_directories(fjimport SYSTEM PRIVATE ${OZZ_ANIM_DIRS})
endif()
get_target_property(OZZ_OFFLINE_DIRS ozz_animation_offline INTERFACE_INCLUDE_DIRECTORIES)
if(OZZ_OFFLINE_DIRS)
    target_include_directories(fjimport SYSTEM PRIVATE ${OZZ_OFFLINE_DIRS})
endif()
