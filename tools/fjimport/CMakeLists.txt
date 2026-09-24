# fjimport — offline importer: glTF/GLB/FBX → .fjmesh, and .fjfont recipes →
# their baked atlases. Links Assimp + meshoptimizer + msdfgen + FreeType.
# NOT linked into the engine runtime.

add_executable(fjimport
    main.cpp
    log_stub.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjfont.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/font_atlas_build.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/font_kerning.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/glyph_outline.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/collision_build.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/meshlet.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/sdf_generator.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/fjmesh_writer.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/material_asset.cpp
)

target_include_directories(fjimport PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(fjimport PRIVATE
    assimp::assimp
    meshoptimizer
    msdfgen::msdfgen-core
    msdfgen::msdfgen-ext
    Freetype::Freetype
    ozz_animation
    ozz_animation_offline
    glm::glm
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Vulkan::Headers
)

if(MSVC)
    target_compile_options(fjimport PRIVATE /W4 /WX)
else()
    target_compile_options(fjimport PRIVATE -Wall -Wextra -Wpedantic ${FJELL_SHADOW_FLAG} -Werror)
endif()

# Treat msdfgen headers as system headers to suppress warnings from them
get_target_property(FJIMPORT_MSDFGEN_CORE_DIRS msdfgen-core INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(fjimport SYSTEM PRIVATE ${FJIMPORT_MSDFGEN_CORE_DIRS})
get_target_property(FJIMPORT_MSDFGEN_EXT_DIRS msdfgen-ext INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(fjimport SYSTEM PRIVATE ${FJIMPORT_MSDFGEN_EXT_DIRS})

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

install(TARGETS fjimport RUNTIME DESTINATION .)
