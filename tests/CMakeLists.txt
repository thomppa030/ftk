# --- Testable engine subset (no Vulkan, no window, no ImGui backends) ---
add_library(fjell-testable STATIC
    ${CMAKE_SOURCE_DIR}/src/animation/animation_state_machine.cpp
    ${CMAKE_SOURCE_DIR}/src/core/clock.cpp
    ${CMAKE_SOURCE_DIR}/src/core/hub_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/math/spline.cpp
    ${CMAKE_SOURCE_DIR}/src/import/import_job.cpp
    ${CMAKE_SOURCE_DIR}/src/import/source_inspection.cpp
    ${CMAKE_SOURCE_DIR}/src/core/input_source.cpp
    ${CMAKE_SOURCE_DIR}/src/core/sdl_input_codes.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/collision_channel.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjui_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjss_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjss_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjfont.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/font_atlas_build.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/font_kerning.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/glyph_outline.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/fjsl_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/fjsl_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/material_gi_params.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/material_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/fjterrain_io.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/fjinst_io.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/fjwater_io.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/water_clipmap.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/water_derivation.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/water_gpu.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/water_spectrum.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/water_surface.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/water_shelter.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/water_shore.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/water_wake.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/components/water_component.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/surface_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/style_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_style_properties.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_data_bind.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_value.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_context.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_pipes.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_models.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_actions.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_animation.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_bar_dynamics.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/widget_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/widget_check.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/widget_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/ui_layout.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjui_serializer.cpp
    ${CMAKE_SOURCE_DIR}/src/core/fixed_timestep.cpp
    ${CMAKE_SOURCE_DIR}/src/core/subsystem.cpp
    ${CMAKE_SOURCE_DIR}/src/core/step_input_latch.cpp
    ${CMAKE_SOURCE_DIR}/src/scripting/test_pacing.cpp
    ${CMAKE_SOURCE_DIR}/src/scripting/timer_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/scripting/script_call.cpp
    ${CMAKE_SOURCE_DIR}/src/scripting/reflected_types.cpp
    ${CMAKE_SOURCE_DIR}/src/scene/scene.cpp
    ${CMAKE_SOURCE_DIR}/src/scene/preset.cpp
    ${CMAKE_SOURCE_DIR}/src/scene/component_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/scene/removal_epoch.cpp
    ${CMAKE_SOURCE_DIR}/src/scene/component_storage.cpp
    ${CMAKE_SOURCE_DIR}/src/scene/component_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/scene/node_type_serializer.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/environment_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/post_layers.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/lut_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/material_track.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/color_gradient.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/track_set.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/day_profile_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/weather_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/weather_player.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/climate_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/climate_director.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/environment_scenario.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/systems/vfx_expression.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/buoyancy.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/collision_shape.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/physics_types.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/physics_world.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/physics_subsystem.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/sdf_generator.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/inspectors/collision_shape_fields.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/asset_kinds.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/folder_colors.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/node_slot.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/node_visuals.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/texture_channels.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/state_graph.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/timeline_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/vfx_graph.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/track_lanes.cpp
)

target_include_directories(fjell-testable PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/scripting
)
target_link_libraries(fjell-testable PUBLIC
    ftk::base
    ftk::math
    ftk::app
    ftk::framegraph
    ftk::shader
    ftk::image
    ftk::app-ui
    ftk::platform
    ftk::imgui-headless
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    glm::glm
    yogacore
    Jolt
)

# physics_world.cpp reaches the renderer's component and asset headers for
# mesh, terrain, and skeletal collision. Only the headers are needed here —
# the test never loads an asset — so their include paths come in as system
# directories, the way the engine target treats them.
foreach(dep Jolt ozz_base ozz_animation)
    get_target_property(dirs ${dep} INTERFACE_INCLUDE_DIRECTORIES)
    if(dirs)
        target_include_directories(fjell-testable SYSTEM PRIVATE ${dirs})
    endif()
endforeach()

# The font baker is tested on a real font, and its test reads the outline
# back through msdfgen, so both see msdfgen and FreeType; msdfgen's headers
# come in as system directories, the way the engine target treats them.
target_link_libraries(fjell-testable PUBLIC msdfgen::msdfgen-core msdfgen::msdfgen-ext Freetype::Freetype
    PRIVATE SDL3::Headers)
foreach(dep msdfgen-core msdfgen-ext)
    get_target_property(dirs ${dep} INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(fjell-testable SYSTEM PUBLIC ${dirs})
endforeach()

ftk_target_defaults(fjell-testable)
if(MSVC)
    target_compile_definitions(fjell-testable PRIVATE FJELL_ENGINE_BUILD)
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
    test_log.cpp
    test_resource_handle.cpp
    test_collision_channel.cpp
    test_delegate.cpp
    test_sdk_delegate.cpp
    test_thread_pool.cpp
    test_fjui_parser.cpp
    test_fjss_parser.cpp
    test_fjsl_loader.cpp
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
    test_font_atlas_build.cpp
    test_font_kerning.cpp
    test_glyph_outline.cpp
    test_utf8.cpp
    test_fjreflect_parser.cpp
    test_fjreflect_generator.cpp
    test_fjreflect_models.cpp
    test_reflected_types.cpp
    ${CMAKE_SOURCE_DIR}/tools/fjreflect/lexer.cpp
    ${CMAKE_SOURCE_DIR}/tools/fjreflect/parser.cpp
    ${CMAKE_SOURCE_DIR}/tools/fjreflect/types.cpp
    ${CMAKE_SOURCE_DIR}/tools/fjreflect/linker.cpp
    ${CMAKE_SOURCE_DIR}/tools/fjreflect/generator.cpp
    test_fjterrain_io.cpp
    test_fjinst_io.cpp
    test_fjwater.cpp
    test_particle_budget.cpp
    test_buoyancy.cpp
    test_fixed_timestep.cpp
    test_fixed_step_blend.cpp
    test_timer_manager.cpp
    test_scene_lifecycle.cpp
    test_step_input_latch.cpp
    test_kinematic_spread.cpp
    test_character_intent.cpp
    test_simulation_determinism.cpp
    test_test_pacing.cpp
    test_water_clipmap.cpp
    test_water_spectrum.cpp
    test_water_shelter.cpp
    test_water_shore.cpp
    test_water_wake.cpp
    test_volume_occupancy.cpp
    test_post_layers.cpp
    test_lut_asset.cpp
    test_post_effects.cpp
    test_ddgi.cpp
    test_ddgi_lights.cpp
    test_rt_hit.cpp
    test_rt_instance.cpp
    test_bindless_index.cpp
    test_surface_asset.cpp
    test_fjui_serializer.cpp
    test_data_bind.cpp
    test_ui_pipes.cpp
    test_ui_paths.cpp
    test_ui_models.cpp
    test_ui_actions.cpp
    test_widget_check.cpp
    test_ui_animation.cpp
    test_ui_bar_dynamics.cpp
    test_spline.cpp
    test_vulkan_access.cpp
    test_vulkan_frame_descriptor_cache.cpp
    test_vulkan_translate.cpp
    test_gpu_access.cpp
    test_frame_graph.cpp
    test_gpu_binding.cpp
    test_gpu_caps.cpp
    test_gpu_command_list.cpp
    test_gpu_release.cpp
    test_gpu_shader.cpp
    test_gpu_texture_view.cpp
    test_gpu_transient_memory.cpp
    test_handle_pool.cpp
    test_small_vector.cpp
    test_clipmap.cpp
    test_character_controller.cpp
    test_sdk_headers.cpp
    test_sdf_generator.cpp
    test_environment.cpp
    test_tracks.cpp
    test_weather.cpp
    test_climate.cpp
    test_environment_scenario.cpp
    test_input_source.cpp
    test_sdl_input_codes.cpp
    test_folder_colors.cpp
    imgui_harness.cpp
    test_text_field.cpp
    test_component_edit_command.cpp
    test_node_edit_command.cpp
    test_component_storage.cpp
    test_component_pool.cpp
    test_node_lifetime.cpp
    test_presets.cpp
    test_script_call.cpp
    test_resource_pool.cpp
    test_scene_node_kinds.cpp
    test_script_pools.cpp
    test_curve_editor.cpp
    test_asset_tile.cpp
    test_choices.cpp
    test_command_group.cpp
    test_edit_record.cpp
    test_scene_order.cpp
    test_search.cpp
    test_status_bar.cpp
    test_dialog.cpp
    test_pages.cpp
    test_document_history.cpp
    test_asset_header.cpp
    test_collision_shape_fields.cpp
    test_tree.cpp
    test_shader_compiler.cpp
    test_shader_diagnostic.cpp
    test_grouped_picker.cpp
    test_node_slot.cpp
    test_slot.cpp
    test_fields.cpp
    test_kit.cpp
    test_asset_kinds.cpp
    test_node_graph.cpp
    test_texture_channels.cpp
    test_list_editor.cpp
    test_track_lanes.cpp
    test_state_graph.cpp
    test_canvas.cpp
    test_timeline.cpp
    test_vfx_expression.cpp
    test_vfx_pin.cpp
    test_vfx_graph.cpp
    test_hub_settings.cpp
    test_source_inspection.cpp
)

target_link_libraries(fjell-tests PRIVATE
    fjell-testable
    Catch2::Catch2WithMain
    SDL3::Headers
)
# The Vulkan backend's own tests (test_vulkan_*.cpp) read its headers.
target_link_libraries(fjell-tests PRIVATE Vulkan::Headers)
# fjreflect's parser is tested in place; it depends on nothing from the engine.
target_include_directories(fjell-tests PRIVATE ${CMAKE_SOURCE_DIR}/tools)
# Tests that need a real font bake the engine's own.
target_compile_definitions(fjell-tests PRIVATE
    FJELL_ENGINE_FONTS_DIR="${CMAKE_SOURCE_DIR}/engine_assets/fonts"
    FJELL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# Tests that read SPIR-V compile shaders of their own from tests/shaders/.
compile_shaders(fjell-tests "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_compile_definitions(fjell-tests PRIVATE
    FJELL_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")

# For a program that links the Vulkan backend and never calls it: the frame
# graph records through gpu::Frame and gpu::CommandList, which the backend
# implements, so linking the frame graph brings the backend and its calls
# into the Vulkan loader. MSVC then loads vulkan-1.dll at the first call
# rather than at startup, so the program runs on a machine without the
# loader, a CI runner among them.
function(fjell_delay_load_vulkan target)
    if(MSVC)
        target_link_options(${target} PRIVATE /DELAYLOAD:vulkan-1.dll)
        target_link_libraries(${target} PRIVATE delayimp)
    endif()
endfunction()

ftk_target_defaults(fjell-tests)
fjell_delay_load_vulkan(fjell-tests)
# Keeps only the shared libraries something references. The backend the
# frame graph brings in still references the Vulkan loader, so on Linux the
# tests need it installed.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(fjell-tests PRIVATE LINKER:--as-needed)
endif()

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(fjell-tests)

# --- Library link checks ---
# One program per library that links that library alone, and whole, so every
# object in it is linked: a definition the library needs from outside itself
# and its links fails here, where the include check cannot see it, rather
# than in the first program outside Fjell that links the library. Each also
# runs as a test, unless it is marked NEEDS_GPU: CI runners have no GPU or
# display, so that one is built everywhere and run by hand.
function(ftk_library_link_check name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "NEEDS_GPU" "" "")
    string(REPLACE "-" "_" file ${name})
    add_executable(ftk-link-${name} library_link/${file}.cpp)
    target_link_libraries(ftk-link-${name} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,ftk::${name}>")
    ftk_target_defaults(ftk-link-${name})
    if(NOT arg_NEEDS_GPU)
        add_test(NAME library_link.${name} COMMAND ftk-link-${name})
    endif()
endfunction()

ftk_library_link_check(base)
ftk_library_link_check(math)
ftk_library_link_check(platform)
ftk_library_link_check(framegraph)
fjell_delay_load_vulkan(ftk-link-framegraph)
ftk_library_link_check(shader)
ftk_library_link_check(image)
ftk_library_link_check(app-ui)
ftk_library_link_check(gpu NEEDS_GPU)
# The GPU link test builds pipelines from the test shaders.
add_dependencies(ftk-link-gpu fjell-tests_shaders)
target_compile_definitions(ftk-link-gpu PRIVATE
    FJELL_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
ftk_library_link_check(app NEEDS_GPU)

# --- Benchmarks ---
# Programs that time part of the engine on a real GPU against the same work
# done straight through Vulkan. Built everywhere, run by hand in an optimised
# build; they print what they measured.
add_executable(fjell-bench-command-list bench_vulkan_command_list.cpp)
target_link_libraries(fjell-bench-command-list PRIVATE ftk::gpu)
ftk_vulkan_backend(fjell-bench-command-list)
add_dependencies(fjell-bench-command-list fjell-tests_shaders)
target_compile_definitions(fjell-bench-command-list PRIVATE
    FJELL_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
ftk_target_defaults(fjell-bench-command-list)
