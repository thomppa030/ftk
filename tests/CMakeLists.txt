# --- Testable engine subset (no Vulkan, no GLFW, no ImGui) ---
# Dear ImGui's core without backends, so editor UI code runs headless in
# tests (imgui_harness.hpp). The engine compiles the same sources together
# with its GLFW and Vulkan backends.
add_library(fjell-imgui-headless STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(fjell-imgui-headless SYSTEM PUBLIC ${imgui_SOURCE_DIR})

add_library(fjell-testable STATIC
    test_log_stub.cpp
    ${CMAKE_SOURCE_DIR}/src/animation/animation_state_machine.cpp
    ${CMAKE_SOURCE_DIR}/src/core/hub_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/import/import_job.cpp
    ${CMAKE_SOURCE_DIR}/src/import/source_inspection.cpp
    ${CMAKE_SOURCE_DIR}/src/core/math/spline.cpp
    ${CMAKE_SOURCE_DIR}/src/core/input_source.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/collision_channel.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/frame_cache_key.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/shader_diagnostic.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjui_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjss_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjss_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/fjfont.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/font_atlas_build.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/font_kerning.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/game_ui/glyph_outline.cpp
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
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/water_wake.cpp
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
    ${CMAKE_SOURCE_DIR}/src/scene/node_type_serializer.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/environment_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/fog_track.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/color_gradient.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/track_set.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/day_profile_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/weather_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/weather_player.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/climate_asset.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/climate_director.cpp
    ${CMAKE_SOURCE_DIR}/src/environment/environment_scenario.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/systems/vfx_curve.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/systems/vfx_expression.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/buoyancy.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/collision_shape.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/physics_types.cpp
    ${CMAKE_SOURCE_DIR}/src/physics/physics_world.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/resources/sdf_generator.cpp
    ${CMAKE_SOURCE_DIR}/src/core/command_history.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/document_history.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/inspectors/collision_shape_fields.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/folder_colors.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/grouped_picker.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/asset_header.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/asset_kind.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/asset_tile.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/brand.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/button.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/canvas.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/curve_editor.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/choice.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/color_field.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/component_block.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/edit_record.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/feedback.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/field.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/inset_group.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/key_cap.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/overlay.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/list_editor.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/menu.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/pane.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/pages.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/row.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/save_bar.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/search.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/section.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/status_bar.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/strip.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/tabs.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/text_field.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/tree.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/kit/viewport_toolbar.cpp
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
    fjell-platform
    fjell-imgui-headless
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

# The font baker is tested on a real font, and its test reads the outline
# back through msdfgen, so both see msdfgen and FreeType; msdfgen's headers
# come in as system directories, the way the engine target treats them.
target_link_libraries(fjell-testable PUBLIC msdfgen::msdfgen-core msdfgen::msdfgen-ext Freetype::Freetype)
foreach(dep msdfgen-core msdfgen-ext)
    get_target_property(dirs ${dep} INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(fjell-testable SYSTEM PUBLIC ${dirs})
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
    test_font_atlas_build.cpp
    test_font_kerning.cpp
    test_glyph_outline.cpp
    test_utf8.cpp
    test_fjreflect_parser.cpp
    ${CMAKE_SOURCE_DIR}/tools/fjreflect/parser.cpp
    test_fjterrain_io.cpp
    test_fjinst_io.cpp
    test_fjwater.cpp
    test_particle_budget.cpp
    test_buoyancy.cpp
    test_water_clipmap.cpp
    test_water_spectrum.cpp
    test_water_wake.cpp
    test_surface_asset.cpp
    test_fjui_serializer.cpp
    test_data_bind.cpp
    test_ui_animation.cpp
    test_ui_bar_dynamics.cpp
    test_spline.cpp
    test_frame_descriptor_cache.cpp
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
    test_folder_colors.cpp
    imgui_harness.cpp
    test_text_field.cpp
    test_component_edit_command.cpp
    test_node_edit_command.cpp
    test_component_storage.cpp
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
    test_shader_diagnostic.cpp
    test_grouped_picker.cpp
    test_fields.cpp
    test_kit.cpp
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
)
# fjreflect's parser is tested in place; it depends on nothing from the engine.
target_include_directories(fjell-tests PRIVATE ${CMAKE_SOURCE_DIR}/tools)
# Tests that need a real font bake the engine's own.
target_compile_definitions(fjell-tests PRIVATE
    FJELL_ENGINE_FONTS_DIR="${CMAKE_SOURCE_DIR}/engine_assets/fonts")

if(MSVC)
    target_compile_options(fjell-tests PRIVATE /W4 /WX)
else()
    target_compile_options(fjell-tests PRIVATE -Wall -Wextra -Wpedantic ${FJELL_SHADOW_FLAG} -Werror)
endif()

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(fjell-tests)
