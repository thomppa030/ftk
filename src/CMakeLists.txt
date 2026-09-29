# --- Main engine ---
add_executable(fjell
    main.cpp
    renderer/accel_structure.cpp
    core/asset_id.cpp
    core/asset_database.cpp
    core/asset_identity.cpp
    core/asset_references.cpp
    core/asset_remap.cpp
    core/asset_type_registry.cpp
    core/base_path.cpp
    core/fixed_timestep.cpp
    core/step_input_latch.cpp
    core/game_builder.cpp
    core/capture_service.cpp
    core/engine.cpp
    core/subsystem.cpp
    environment/environment_resolver.cpp
    environment/environment_fixed.cpp
    environment/material_track.cpp
    environment/material_track_scene.cpp
    environment/environment_subsystem.cpp
    environment/environment_overrides.cpp
    environment/environment_scenario.cpp
    environment/scenario_player.cpp
    environment/color_gradient.cpp
    environment/track_set.cpp
    environment/day_profile_asset.cpp
    environment/weather_asset.cpp
    environment/weather_player.cpp
    environment/climate_asset.cpp
    environment/climate_director.cpp
    ui/track_lanes.cpp
    ui/day_profile_editor_context.cpp
    ui/water_editor_context.cpp
    ui/weather_editor_context.cpp
    ui/track_asset_editor_context.cpp
    core/input.cpp
    core/input_action_map.cpp
    core/input_context_io.cpp
    core/input_modifier.cpp
    core/input_source.cpp
    core/sdl_input_codes.cpp
    core/play_controller.cpp
    core/play_snapshot.cpp
    core/project.cpp
    core/hub_settings.cpp
    import/import_job.cpp
    import/import_dialog.cpp
    import/source_inspection.cpp
    renderer/camera.cpp
    renderer/debug_draw.cpp
    renderer/draw_data_descriptor.cpp
    renderer/fly_camera.cpp
    renderer/indirect_batch.cpp
    renderer/instance_provider.cpp
    renderer/resources/material.cpp
    renderer/resources/fjmesh_writer.cpp
    renderer/resources/asset_shader_type.cpp
    renderer/resources/material_asset.cpp
    renderer/resources/material_gi_params.cpp
    renderer/resources/material_instance.cpp
    renderer/components/mesh_component.cpp
    renderer/components/water_component.cpp
    renderer/systems/vfx_asset.cpp
    renderer/systems/vfx_module.cpp
    renderer/systems/vfx_emitter.cpp
    renderer/systems/vfx_expression.cpp
    renderer/systems/vfx_expression_eval.cpp
    renderer/resources/mega_buffer.cpp
    renderer/resources/meshlet.cpp
    renderer/resources/sdf_generator.cpp
    renderer/resources/meshlet_buffer.cpp
    renderer/resources/meshlet_descriptor.cpp
    renderer/resources/model.cpp
    renderer/offscreen_pass.cpp
    renderer/scene_viewport.cpp
    renderer/pipeline.cpp
    renderer/post_process_resources.cpp
    renderer/post_layers.cpp
    renderer/pipeline_cache.cpp
    renderer/resources/primitives.cpp
    renderer/bindless.cpp
    renderer/systems/particle_system.cpp
    renderer/systems/vfx_subsystem.cpp
    renderer/systems/sprite_renderer.cpp
    renderer/systems/sprite_sheet.cpp
    renderer/systems/sprite_anim_subsystem.cpp
    renderer/systems/tilemap_renderer.cpp
    renderer/pass/render_pass.cpp
    renderer/pass/bloom_pass.cpp
    renderer/pass/instance_cull_pass.cpp
    renderer/pass/ddgi_pass.cpp
    renderer/pass/ss_gi_pass.cpp
    renderer/pass/gtao_pass.cpp
    renderer/pass/ssr_pass.cpp
    renderer/pass/rt_reflection_pass.cpp
    renderer/pass/volumetric_fog_pass.cpp
    renderer/pass/sun_shafts_pass.cpp
    renderer/pass/contact_shadow_pass.cpp
    renderer/pass/cloud_shadow_pass.cpp
    renderer/components/motion_source_component_meta.cpp
    renderer/pass/motion_field_pass.cpp
    renderer/pass/fog_density_field_pass.cpp
    renderer/pass/water_fft_pass.cpp
    renderer/pass/grid_pass.cpp
    renderer/pass/debug_pass.cpp
    renderer/pass/output_overlay.cpp
    renderer/pass/depth_prepass.cpp
    renderer/pass/depth_variants.cpp
    renderer/pass/depth_resolve_pass.cpp
    renderer/pass/depth_viz_pass.cpp
    renderer/pass/light_cull_pass.cpp
    renderer/pass/mesh_shader_depth_pass.cpp
    renderer/pass/hiz_pass.cpp
    renderer/pass/meshlet_cull_pass2.cpp
    renderer/pass/meshlet_instance_pass.cpp
    renderer/pass/meshlet_viz_pass.cpp
    renderer/pass/second_cull_depth_pass.cpp
    renderer/pass/light_viz_pass.cpp
    renderer/pass/shadow_pass.cpp
    renderer/pass/sky_pass.cpp
    renderer/pass/sky_cubemap_pass.cpp
    renderer/pass/local_shadow_pass.cpp
    renderer/pass/taa_resolve_pass.cpp
    renderer/pass/terrain_pass.cpp
    renderer/pass/water_pass.cpp
    renderer/pass/post_pass.cpp
    renderer/pass/transparent_pass.cpp
    renderer/pass/normal_pass.cpp
    renderer/pass/overdraw_pass.cpp
    renderer/pass/uv_viz_pass.cpp
    renderer/pass/viz_meshes.cpp
    renderer/pass/wireframe_pass.cpp
    renderer/pass/scene_sdf_pass.cpp
    renderer/pass/opaque_pass.cpp
    renderer/pass/lit_surface_reads.cpp
    renderer/pass/screen_color_copy_pass.cpp
    renderer/pass/distortion_pass.cpp
    renderer/pass/distortion_apply_pass.cpp
    renderer/pass/exposure_pass.cpp
    renderer/pass/subsystem_compute_pass.cpp
    renderer/components/arrow_component_meta.cpp
    renderer/components/mesh_component_meta.cpp
    renderer/components/spline_component_meta.cpp
    renderer/components/spline_mesh_component_meta.cpp
    renderer/components/instanced_mesh_component.cpp
    renderer/components/instanced_mesh_component_meta.cpp
    renderer/components/skeletal_mesh_component_meta.cpp
    renderer/components/particle_emitter_meta.cpp
    renderer/components/sprite_component_meta.cpp
    renderer/components/sprite_anim_component_meta.cpp
    renderer/components/tilemap_component_meta.cpp
    renderer/components/terrain_component_meta.cpp
    renderer/components/water_component_meta.cpp
    renderer/resources/fjterrain_io.cpp
    renderer/resources/fjwater_io.cpp
    renderer/resources/water_clipmap.cpp
    renderer/resources/water_derivation.cpp
    renderer/resources/water_gpu.cpp
    renderer/resources/water_spectrum.cpp
    renderer/resources/water_surface.cpp
    renderer/resources/water_wake.cpp
    renderer/resources/terrain_asset.cpp
    renderer/render_queue.cpp
    renderer/resources/asset_loader.cpp
    renderer/resources/resource_manager.cpp
    renderer/resources/skeletal_model.cpp
    renderer/resources/lut_asset.cpp
    renderer/resources/texture.cpp
    renderer/viz_image.cpp
    renderer/gpu_context.cpp
    renderer/global_bindings.cpp
    renderer/fjgraph_parser.cpp
    renderer/fjgraph_watcher.cpp
    renderer/pass_factory.cpp
    renderer/pass_registrations.cpp
    renderer/pass_registry.cpp
    renderer/pass_setup.cpp
    renderer/render_pipeline.cpp
    renderer/render_settings.cpp
    renderer/scene_gatherer.cpp
    renderer/vulkan_context.cpp
    animation/skeleton.cpp
    animation/animation_clip.cpp
    animation/animation_state_machine.cpp
    animation/animation_system.cpp
    animation/animator_component.cpp
    animation/anim_notify_registry.cpp
    animation/builtin_notifies.cpp
    animation/blend_space.cpp
    animation/animator_component_meta.cpp
    animation/bone_buffer.cpp
    audio/audio_component_meta.cpp
    audio/audio_asset.cpp
    audio/audio_engine.cpp
    audio/audio_subsystem.cpp
    physics/rigidbody_component_meta.cpp
    physics/collider_component_meta.cpp
    physics/character_component_meta.cpp
    physics/physics_types.cpp
    physics/surface_asset.cpp
    physics/collision_channel.cpp
    physics/collision_build.cpp
    physics/buoyancy.cpp
    physics/buoyancy_component_meta.cpp
    physics/collision_shape.cpp
    physics/physics_world.cpp
    physics/physics_subsystem.cpp
    scene/scene.cpp
    scene/scene_io.cpp
    scene/node_serializer.cpp
    scene/node_type_serializer.cpp
    scene/spline_editable.cpp
    scene/preset.cpp
    scene/preset_file.cpp
    scene/camera_subsystem.cpp
    scene/component_registry.cpp
    scene/component_pool.cpp
    scene/removal_epoch.cpp
    scene/component_storage.cpp
    core/input_component_meta.cpp
    scripting/script_manager.cpp
    scripting/script_call.cpp
    scripting/reflected_types.cpp
    scripting/test_pacing.cpp
    scripting/test_runner.cpp
    scripting/script_globals.cpp
    scripting/asset_type_registrations.cpp
    scripting/log_api.cpp
    scripting/material_api.cpp
    scripting/audio_api.cpp
    scripting/camera_api.cpp
    scripting/input_api.cpp
    scripting/animator_api.cpp
    scripting/vfx_api.cpp
    scripting/physics_api.cpp
    scripting/surface_api.cpp
    scripting/water_api.cpp
    scripting/viewport_api.cpp
    scripting/timer_manager.cpp
    scripting/timer_api.cpp
    scripting/time_api.cpp
    scripting/environment_api.cpp
    scripting/debug_draw_queue.cpp
    scripting/screen_debug_queue.cpp
    scripting/debug_api.cpp
    scripting/scene_api.cpp
    scripting/net_api.cpp
    net/enet_transport.cpp
    net/net_manager.cpp
    net/prediction_state.cpp
    net/spawn_replicator.cpp
    net/rpc_router.cpp
    net/replication_graph.cpp
    net/net_subsystem.cpp
    ui/asset_picker.cpp
    ui/asset_ref_widget.cpp
    ui/node_slot.cpp
    ui/node_visuals.cpp
    ui/capture_popup.cpp
    ui/menu_bar.cpp
    core/project_manager.cpp
    ui/content_browser.cpp
    ui/folder_colors.cpp
    ui/hierarchy_panel.cpp
    ui/texture_channels.cpp
    ui/texture_preview.cpp
    ui/sdf_debug_panel.cpp
    ui/properties_node_sections.cpp
    ui/properties_panel.cpp
    ui/shader_error_callout.cpp
    ui/stats_panel.cpp
    ui/net_panel.cpp
    ui/project_settings_window.cpp
    ui/engine_editor_context.cpp
    ui/animation_editor_context.cpp
    ui/blend_space_editor_context.cpp
    ui/input_mapping_editor_context.cpp
    ui/material_editor_context.cpp
    ui/asset_thumbnail_renderer.cpp
    ui/mesh_editor_context.cpp
    ui/font_editor_context.cpp
    ui/ui_editor_context.cpp
    ui/ui_preview_data.cpp
    ui/vfx_editor_context.cpp
    ui/inspectors/animation_inspectors.cpp
    ui/inspectors/asset_rows.cpp
    ui/inspectors/collision_shape_fields.cpp
    ui/inspectors/vfx_module_inspectors.cpp
    ui/inspectors/collision_filter_inspector.cpp
    ui/inspectors/environment_inspectors.cpp
    ui/inspectors/gameplay_inspectors.cpp
    ui/inspectors/inspector_registry.cpp
    ui/inspectors/physics_inspectors.cpp
    ui/inspectors/rendering_inspectors.cpp
    ui/inspectors/script_fields.cpp
    ui/inspectors/model_fields.cpp
    ui/inspectors/spline_inspectors.cpp
    ui/material_params_widget.cpp
    ui/scene_editor_context.cpp
    ui/brush.cpp
    ui/terrain_sculpt.cpp
    ui/dockspace_layout.cpp
    ui/state_graph.cpp
    ui/vfx_graph.cpp
    ui/timeline_widget.cpp
    ui/viewport_manager.cpp
    ui/viewport_view.cpp
    ui/game_ui/fjui_parser.cpp
    ui/game_ui/fjss_parser.cpp
    ui/game_ui/fjui_serializer.cpp
    ui/game_ui/style_resolver.cpp
    ui/game_ui/fjss_loader.cpp
    ui/game_ui/ui_style_properties.cpp
    ui/game_ui/ui_layout.cpp
    ui/game_ui/ui_renderer.cpp
    ui/game_ui/ui_input.cpp
    ui/game_ui/ui_data_bind.cpp
    ui/game_ui/ui_value.cpp
    ui/game_ui/ui_context.cpp
    ui/game_ui/ui_pipes.cpp
    ui/game_ui/ui_models.cpp
    ui/game_ui/ui_actions.cpp
    ui/game_ui/ui_animation.cpp
    ui/game_ui/ui_bar_dynamics.cpp
    ui/game_ui/ui_watcher.cpp
    ui/game_ui/widget_resolver.cpp
    ui/game_ui/ui_subsystem.cpp
    ui/game_ui/game_model_world.cpp
    ui/game_ui/ui_widget_models.cpp
    ui/game_ui/ui_widget_component_meta.cpp
    ui/game_ui/widget_asset.cpp
    ui/game_ui/widget_check.cpp
    ui/game_ui/game_ui.cpp
    ui/game_ui/fjfont.cpp
    ui/game_ui/font_atlas.cpp
    ui/game_ui/font_library.cpp
    ui/game_ui/font_sample.cpp
    ui/game_ui/text_shaper.cpp

    # ImGuizmo
    ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp
)

target_include_directories(fjell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/scripting
)

target_include_directories(fjell SYSTEM PRIVATE
    ${imguizmo_SOURCE_DIR}/src
    ${miniaudio_SOURCE_DIR}
)

# Fails the build on simulation code that reads the frame. The editor UI
# check is added from the top level, once this target exists.
add_dependencies(fjell fjell-fixed-tick-check)

# Libraries link whole: a static link keeps only the objects the engine calls
# into, and game modules can be handed (-rdynamic, ENABLE_EXPORTS) only what
# is linked.
target_link_libraries(fjell PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,fjell-core>"
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,fjell-platform>"
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,fjell-gpu>"
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,fjell-framegraph>"
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,fjell-shader>"
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,fjell-stb>"
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,fjell-ui-kit>"
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,fjell-editor-shell>"
    fjell-imgui
    fastgltf::fastgltf
    glm::glm
    GPUOpen::VulkanMemoryAllocator
    Jolt
    nlohmann_json::nlohmann_json
    SDL3::SDL3
    spdlog::spdlog
    Vulkan::Vulkan
    yogacore
    meshoptimizer
    ozz_animation
    ozz_animation_offline
    enet
    ${CMAKE_DL_LIBS}
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(fjell PRIVATE -rdynamic)
endif()

# Export symbols from the engine exe so game script DLLs can link against them.
# On Linux this is handled by -rdynamic above.
# On Windows, ENABLE_EXPORTS generates fjell.lib import library; symbols are
# marked for export via FJELL_SDK_EXPORT in the SDK headers.
set_target_properties(fjell PROPERTIES ENABLE_EXPORTS TRUE)

# The SDK headers are shared with game modules; this tells them they are being
# compiled into the engine itself (symbol export direction, and which headers
# would collide with the engine's own types).
target_compile_definitions(fjell PRIVATE FJELL_ENGINE_BUILD)

fjell_target_defaults(fjell)
if(MSVC)
    # Suppress warnings in third-party ImGuizmo source
    set_source_files_properties(${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp PROPERTIES COMPILE_FLAGS "/W0")
else()
    set_source_files_properties(${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp PROPERTIES COMPILE_FLAGS "-w")
    # miniaudio (vendored, included from our .cpp wrapper) makes GCC report a
    # bogus out-of-bounds inside <vector>'s move path once the optimiser is on,
    # so it only appears in release builds.
    set_source_files_properties(audio/audio_engine.cpp PROPERTIES COMPILE_FLAGS "-Wno-error=array-bounds")
    # collect_subtree() appends to the vector it walks, and GCC 13 at -O3 reads
    # the range insert as writing into a zero-sized region. Not reproducible on
    # GCC 16, and the traversal is bounded by the parent map it reads from.
    set_source_files_properties(scene/scene.cpp PROPERTIES COMPILE_FLAGS "-Wno-error=stringop-overflow")
endif()

# Treat fastgltf headers as system headers to suppress warnings from them
get_target_property(FASTGLTF_INCLUDE_DIRS fastgltf::fastgltf INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(fjell SYSTEM PRIVATE ${FASTGLTF_INCLUDE_DIRS})

# Treat Jolt headers as system headers to suppress warnings from them
get_target_property(JOLT_INCLUDE_DIRS Jolt INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(fjell SYSTEM PRIVATE ${JOLT_INCLUDE_DIRS})

# Treat Yoga headers as system headers to suppress warnings from them
get_target_property(YOGA_INCLUDE_DIRS yogacore INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(fjell SYSTEM PRIVATE ${YOGA_INCLUDE_DIRS})

# Treat ozz-animation headers as system headers to suppress warnings from them
get_target_property(OZZ_BASE_DIRS ozz_base INTERFACE_INCLUDE_DIRECTORIES)
if(OZZ_BASE_DIRS)
    target_include_directories(fjell SYSTEM PRIVATE ${OZZ_BASE_DIRS})
endif()
get_target_property(OZZ_ANIM_DIRS ozz_animation INTERFACE_INCLUDE_DIRECTORIES)
if(OZZ_ANIM_DIRS)
    target_include_directories(fjell SYSTEM PRIVATE ${OZZ_ANIM_DIRS})
endif()
get_target_property(OZZ_OFFLINE_DIRS ozz_animation_offline INTERFACE_INCLUDE_DIRECTORIES)
if(OZZ_OFFLINE_DIRS)
    target_include_directories(fjell SYSTEM PRIVATE ${OZZ_OFFLINE_DIRS})
endif()

# Baking font atlases needs FreeType and msdfgen. A development build bakes
# a stale atlas itself; a shipped build reads only what fjimport baked, and
# links neither library. Release drops it by default; any other
# configuration keeps it.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_font_bake_default OFF)
else()
    set(_font_bake_default ON)
endif()
option(FJELL_FONT_BAKE "Bake font atlases in the engine (links FreeType and msdfgen)" ${_font_bake_default})
if(FJELL_FONT_BAKE)
    target_sources(fjell PRIVATE ui/game_ui/font_atlas_build.cpp ui/game_ui/font_kerning.cpp ui/game_ui/glyph_outline.cpp)
    target_compile_definitions(fjell PRIVATE FJELL_FONT_BAKE=1)
    target_link_libraries(fjell PRIVATE msdfgen::msdfgen-core msdfgen::msdfgen-ext Freetype::Freetype)
    # Treat msdfgen headers as system headers to suppress warnings from them
    get_target_property(MSDFGEN_CORE_DIRS msdfgen-core INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(fjell SYSTEM PRIVATE ${MSDFGEN_CORE_DIRS})
    get_target_property(MSDFGEN_EXT_DIRS msdfgen-ext INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(fjell SYSTEM PRIVATE ${MSDFGEN_EXT_DIRS})
endif()

set(SHADER_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
compile_shaders(fjell ${SHADER_DIR} ${SHADER_OUTPUT_DIR})

# Mirror the shaders source tree into the build dir at every build via a
# small inline cmake -P script. Using a script (not a one-shot file(GLOB) at
# configure time) means newly-added .fjsl files and nested include/*/*.glsl
# get picked up without re-running cmake. The script copies-if-different so
# unchanged files don't cause incremental retriggering.
add_custom_target(fjell_shader_resources ALL
    COMMAND ${CMAKE_COMMAND}
            -DSHADER_SRC=${SHADER_DIR}
            -DSHADER_DST=${SHADER_OUTPUT_DIR}
            -P ${CMAKE_SOURCE_DIR}/cmake/MirrorShaderSources.cmake
    COMMENT "Mirroring FJSL shaders and includes to build directory"
)
add_dependencies(fjell fjell_shader_resources)

# Symlink assets into build dir so the executable finds them during development.
# COPY_ON_ERROR: falls back to a full copy if symlinks aren't available
# (e.g. Windows without Developer Mode or admin privileges).
file(CREATE_LINK "${CMAKE_SOURCE_DIR}/assets" "${CMAKE_BINARY_DIR}/assets" SYMBOLIC COPY_ON_ERROR)
file(CREATE_LINK "${CMAKE_SOURCE_DIR}/engine_assets" "${CMAKE_BINARY_DIR}/engine_assets" SYMBOLIC COPY_ON_ERROR)

# SDK headers live in src/scripting/fjell/ — projects include directly via
# -I<engine_src>/src/scripting, so #include <fjell/component.hpp> works.

# --- Fjell Hub (launcher) ---
add_executable(fjell-hub
    hub/main.cpp
    hub/hub_ui.cpp
    hub/recent_projects.cpp
    core/hub_settings.cpp
)

target_include_directories(fjell-hub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
# The hub draws its ImGui through the same sRGB-decoding fragment stage.
add_dependencies(fjell-hub fjell_shaders)

target_link_libraries(fjell-hub PRIVATE
    fjell-core
    fjell-editor-shell
    fjell-gpu
    fjell-platform
    fjell-ui-kit
    fjell-imgui
    glm::glm
    GPUOpen::VulkanMemoryAllocator
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Vulkan::Vulkan
)
fjell_target_defaults(fjell-hub)

# --- Install rules ---
# ARCHIVE carries MSVC's import library: ENABLE_EXPORTS emits fjell.lib beside
# fjell.exe, and generated game projects link against it to resolve engine symbols.
install(TARGETS fjell fjell-hub
        RUNTIME DESTINATION .
        ARCHIVE DESTINATION .)
install(DIRECTORY "${SHADER_OUTPUT_DIR}/" DESTINATION shaders
        FILES_MATCHING PATTERN "*.spv")
# The FJSL material system generates and compiles GLSL at runtime, so the
# templates it injects and the .fjsl sources have to ship alongside the SPIR-V.
install(DIRECTORY "${CMAKE_SOURCE_DIR}/shaders/include" DESTINATION shaders
        FILES_MATCHING PATTERN "*.glsl")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/shaders/" DESTINATION shaders
        FILES_MATCHING PATTERN "*.fjsl")
# The assets directory holds local-only development screenshots the
# repository ignores, which would otherwise be packaged from a working tree
# even though a fresh checkout never has them.
install(DIRECTORY "${CMAKE_SOURCE_DIR}/assets/" DESTINATION assets
        PATTERN "screenshots" EXCLUDE)
# A shipped engine cannot bake, so every engine font is baked here, into the
# source tree's cache, before the directory is copied. fjimport is built by
# the same configuration, whatever FJELL_FONT_BAKE says.
install(CODE "
    file(GLOB _recipes \"${CMAKE_SOURCE_DIR}/engine_assets/fonts/*.fjfont\")
    foreach(_recipe IN LISTS _recipes)
        message(STATUS \"Baking \${_recipe}\")
        execute_process(COMMAND \"$<TARGET_FILE:fjimport>\" \"\${_recipe}\"
                        RESULT_VARIABLE _bake_result)
        if(NOT _bake_result EQUAL 0)
            message(FATAL_ERROR \"font bake failed for \${_recipe}\")
        endif()
    endforeach()
")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/engine_assets/" DESTINATION engine_assets)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/scripting/fjell" DESTINATION sdk)
