# --- Testable engine subset (no Vulkan, no GLFW, no ImGui) ---
add_library(fjell-testable STATIC
    test_log_stub.cpp
    ${CMAKE_SOURCE_DIR}/src/core/math/spline.cpp
    ${CMAKE_SOURCE_DIR}/src/core/input_source.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/collision_channel.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/frame_cache_key.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjui_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjss_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjss_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjfont.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/fjsl_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/material_gi_params.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/material_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/fjterrain_io.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/fjinst_io.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/surface_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/style_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_style_properties.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_data_bind.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_animation.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_bar_dynamics.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_widget_context.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/widget_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/widget_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_layout.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjui_serializer.cpp
    ${CMAKE_SOURCE_DIR}/src/core/thread_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/scene/scene.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/environment_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/color_gradient.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/track_set.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/day_profile_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/weather_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/weather_player.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/climate_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/climate_director.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/systems/vfx_curve.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/collision_shape.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/physics_types.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/physics_world.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/sdf_generator.cpp
)

target_include_directories(fjell-testable PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/scripting
)
target_link_libraries(fjell-testable PUBLIC
    fjell-platform
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    glm::glm
    Vulkan::Headers
    yogacore
    Jolt
)

# physics_world.cpp reaches the renderer's component and asset headers for
# mesh, terrain, and skeletal collision. Only the headers are needed here —
# the test never loads an asset — so their include paths come in as system
# directories, the way the engine target treats them.
foreach(dep Jolt GPUOpen::VulkanMemoryAllocator ozz_base ozz_animation)
    get_target_property(dirs ${dep} INTERFACE_INCLUDE_DIRECTORIES)
    if(dirs)
        target_include_directories(fjell-testable SYSTEM PRIVATE ${dirs})
    endif()
endforeach()

if(MSVC)
    target_compile_options(fjell-testable PRIVATE /W4 /WX)
    target_compile_definitions(fjell-testable PRIVATE FJELL_ENGINE_BUILD)
else()
    target_compile_options(fjell-testable PRIVATE -Wall -Wextra -Wpedantic ${FJELL_SHADOW_FLAG} -Werror)
endif()

# Source file properties are scoped to the directory that set them, so an
# engine source with a targeted warning suppression in src/ compiles here
# without it and can fail under -Werror in this target alone. Every engine
# source in the library keeps the per-file flags src/ gave it.
get_target_property(FJELL_TESTABLE_SOURCES fjell-testable SOURCES)
foreach(source IN LISTS FJELL_TESTABLE_SOURCES)
    if(NOT source MATCHES "^${CMAKE_SOURCE_DIR}/src/")
        continue()
    endif()
    get_source_file_property(flags "${source}" DIRECTORY "${CMAKE_SOURCE_DIR}/src" COMPILE_FLAGS)
    if(flags)
        set_source_files_properties("${source}" PROPERTIES COMPILE_FLAGS "${flags}")
    endif()
endforeach()

# --- Test executable ---
add_executable(fjell-tests
    test_resource_handle.cpp
    test_collision_channel.cpp
    test_delegate.cpp
    test_fjui_parser.cpp
    test_fjss_parser.cpp
    test_fjsl_parser.cpp
    test_material_gi_params.cpp
    test_color_space.cpp
    test_material_asset.cpp
    test_style_resolver.cpp
    test_ui_style_properties.cpp
    test_widget_resolver.cpp
    test_ui_layout.cpp
    test_fjss_serializer.cpp
    test_fjss_loader.cpp
    test_fjfont.cpp
    test_fjterrain_io.cpp
    test_fjinst_io.cpp
    test_surface_asset.cpp
    test_fjui_serializer.cpp
    test_data_bind.cpp
    test_ui_animation.cpp
    test_ui_bar_dynamics.cpp
    test_spline.cpp
    test_frame_descriptor_cache.cpp
    test_ddgi_clipmap.cpp
    test_character_controller.cpp
    test_sdk_headers.cpp
    test_sdf_generator.cpp
    test_environment.cpp
    test_tracks.cpp
    test_weather.cpp
    test_climate.cpp
    test_input_source.cpp
)

target_link_libraries(fjell-tests PRIVATE
    fjell-testable
    Catch2::Catch2WithMain
)

if(MSVC)
    target_compile_options(fjell-tests PRIVATE /W4 /WX)
else()
    target_compile_options(fjell-tests PRIVATE -Wall -Wextra -Wpedantic ${FJELL_SHADOW_FLAG} -Werror)
endif()

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(fjell-tests)
