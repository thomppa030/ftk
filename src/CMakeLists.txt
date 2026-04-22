# --- Platform abstraction (shared between fjell and fjell-hub) ---
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(FJELL_PLATFORM_SOURCES
        platform/linux/platform_linux.cpp
        platform/linux/file_watcher_linux.cpp
        platform/linux/shared_library_linux.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(FJELL_PLATFORM_SOURCES
        platform/windows/platform_windows.cpp
        platform/windows/file_watcher_windows.cpp
        platform/windows/shared_library_windows.cpp
    )
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

add_library(fjell-platform STATIC ${FJELL_PLATFORM_SOURCES})
target_include_directories(fjell-platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fjell-platform PRIVATE spdlog::spdlog ${CMAKE_DL_LIBS})
if(MSVC)
    target_compile_options(fjell-platform PRIVATE /W4 /WX)
else()
    target_compile_options(fjell-platform PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# --- Main engine ---
add_executable(fjell
    main.cpp
    renderer/gpu/accel_structure.cpp
    renderer/gpu/vma_impl.cpp
    core/asset_id.cpp
    core/asset_database.cpp
    core/base_path.cpp
    core/clock.cpp
    core/game_builder.cpp
    core/engine.cpp
    core/layer_stack.cpp
    core/math/spline.cpp
    core/input.cpp
    core/input_action_map.cpp
    core/input_context_io.cpp
    core/input_modifier.cpp
    core/input_source.cpp
    core/play_session.cpp
    core/project.cpp
    core/thread_pool.cpp
    core/log.cpp
    core/window.cpp
    import/import_job.cpp
    import/import_dialog.cpp
    renderer/gpu/buffer.cpp
    renderer/camera.cpp
    renderer/debug_draw.cpp
    renderer/gpu/descriptor.cpp
    renderer/gpu/draw_data_descriptor.cpp
    renderer/fly_camera.cpp
    renderer/gpu/device.cpp
    renderer/gpu/gpu_core.cpp
    renderer/gpu/image.cpp
    renderer/indirect_batch.cpp
    renderer/instance_provider.cpp
    renderer/resources/fjsl_parser.cpp
    renderer/resources/material.cpp
    renderer/resources/material_asset.cpp
    renderer/systems/vfx_asset.cpp
    renderer/systems/vfx_curve.cpp
    renderer/systems/vfx_module.cpp
    renderer/systems/vfx_emitter.cpp
    renderer/systems/vfx_expression.cpp
    renderer/systems/vfx_expression_eval.cpp
    renderer/resources/mega_buffer.cpp
    renderer/resources/mesh.cpp
    renderer/resources/meshlet.cpp
    renderer/resources/sdf_generator.cpp
    renderer/resources/meshlet_buffer.cpp
    renderer/resources/meshlet_descriptor.cpp
    renderer/resources/model.cpp
    renderer/offscreen_pass.cpp
    renderer/scene_viewport.cpp
    renderer/gpu/pipeline.cpp
    renderer/render_target_pool.cpp
    renderer/post_process_resources.cpp
    renderer/pipeline_cache.cpp
    renderer/resources/primitives.cpp
    renderer/bindless.cpp
    renderer/frame_graph.cpp
    renderer/frame_cache_key.cpp
    renderer/frame_descriptor_cache.cpp
    renderer/transient_image_pool.cpp
    renderer/pass_builder.cpp
    renderer/gpu/thread_command_pools.cpp
    renderer/systems/particle_system.cpp
    renderer/systems/vfx_subsystem.cpp
    renderer/systems/sprite_renderer.cpp
    renderer/systems/sprite_sheet.cpp
    renderer/systems/sprite_anim_subsystem.cpp
    renderer/systems/tilemap_renderer.cpp
    renderer/pass/render_pass.cpp
    renderer/pass/bloom_pass.cpp
    renderer/pass/cull_pass.cpp
    renderer/pass/instance_cull_pass.cpp
    renderer/pass/ddgi_pass.cpp
    renderer/pass/ss_gi_pass.cpp
    renderer/pass/cull_viz_pass.cpp
    renderer/pass/gtao_pass.cpp
    renderer/pass/ssr_pass.cpp
    renderer/pass/volumetric_fog_pass.cpp
    renderer/pass/sun_shafts_pass.cpp
    renderer/pass/contact_shadow_pass.cpp
    renderer/pass/grid_pass.cpp
    renderer/pass/debug_pass.cpp
    renderer/pass/depth_prepass.cpp
    renderer/pass/depth_resolve_pass.cpp
    renderer/pass/depth_viz_pass.cpp
    renderer/pass/light_cull_pass.cpp
    renderer/pass/mesh_shader_depth_pass.cpp
    renderer/pass/hiz_pass.cpp
    renderer/pass/meshlet_cull_pass2.cpp
    renderer/pass/meshlet_instance_pass.cpp
    renderer/pass/meshlet_viz_pass.cpp
    renderer/pass/light_viz_pass.cpp
    renderer/pass/shadow_pass.cpp
    renderer/pass/sky_pass.cpp
    renderer/pass/sky_cubemap_pass.cpp
    renderer/pass/local_shadow_pass.cpp
    renderer/pass/taa_resolve_pass.cpp
    renderer/pass/tonemap_pass.cpp
    renderer/pass/transparent_pass.cpp
    renderer/pass/normal_pass.cpp
    renderer/pass/overdraw_pass.cpp
    renderer/pass/uv_viz_pass.cpp
    renderer/pass/wireframe_pass.cpp
    renderer/pass/scene_sdf_pass.cpp
    renderer/pass/opaque_pass.cpp
    renderer/pass/screen_color_copy_pass.cpp
    renderer/pass/distortion_pass.cpp
    renderer/pass/distortion_apply_pass.cpp
    renderer/pass/exposure_pass.cpp
    renderer/pass/subsystem_compute_pass.cpp
    renderer/components/arrow_component_meta.cpp
    renderer/components/mesh_component_meta.cpp
    renderer/components/spline_component_meta.cpp
    renderer/components/spline_mesh_component_meta.cpp
    renderer/components/instanced_mesh_component_meta.cpp
    renderer/components/skeletal_mesh_component_meta.cpp
    renderer/components/particle_emitter_meta.cpp
    renderer/components/sprite_component_meta.cpp
    renderer/components/sprite_anim_component_meta.cpp
    renderer/components/tilemap_component_meta.cpp
    renderer/render_queue.cpp
    renderer/resources/asset_loader.cpp
    renderer/resources/resource_manager.cpp
    renderer/shader_watcher.cpp
    renderer/resources/skeletal_model.cpp
    renderer/resources/stb_image.cpp
    renderer/resources/stb_image_resize.cpp
    renderer/resources/stb_image_write.cpp
    renderer/gpu/swapchain.cpp
    renderer/resources/texture.cpp
    renderer/viz_image.cpp
    renderer/gpu_context.cpp
    renderer/global_bindings.cpp
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
    physics/physics_world.cpp
    scene/scene.cpp
    scene/node_serializer.cpp
    scene/spline_editable.cpp
    scene/preset.cpp
    scene/camera_subsystem.cpp
    scene/component_registry.cpp
    core/input_component_meta.cpp
    scripting/script_manager.cpp
    scripting/script_globals.cpp
    scripting/asset_type_registry.cpp
    scripting/asset_type_registrations.cpp
    scripting/log_api.cpp
    scripting/audio_api.cpp
    scripting/camera_api.cpp
    scripting/input_api.cpp
    scripting/animator_api.cpp
    scripting/sprite_anim_api.cpp
    scripting/vfx_api.cpp
    scripting/viewport_api.cpp
    scripting/timer_manager.cpp
    scripting/timer_api.cpp
    scripting/time_api.cpp
    scripting/debug_draw_queue.cpp
    scripting/debug_api.cpp
    scripting/scene_api.cpp
    ui/asset_ref_widget.cpp
    ui/console.cpp
    ui/menu_bar.cpp
    ui/file_browser.cpp
    ui/imgui_layer.cpp
    core/project_manager.cpp
    core/command_history.cpp
    ui/content_browser.cpp
    ui/hierarchy_panel.cpp
    ui/history_panel.cpp
    ui/icon_cache.cpp
    ui/properties_panel.cpp
    ui/stats_panel.cpp
    ui/node_editor_panel.cpp
    ui/editor_context.cpp
    ui/animation_editor_context.cpp
    ui/blend_space_1d_editor_context.cpp
    ui/blend_space_2d_editor_context.cpp
    ui/context_registry.cpp
    ui/input_mapping_editor_context.cpp
    ui/material_editor_context.cpp
    ui/ui_editor_context.cpp
    ui/vfx_editor_context.cpp
    ui/imgui_curve_editor.cpp
    ui/material_params_widget.cpp
    ui/scene_editor_context.cpp
    ui/dockspace_layout.cpp
    ui/timeline_widget.cpp
    ui/viewport_manager.cpp
    ui/game_ui/fjui_parser.cpp
    ui/game_ui/fjss_parser.cpp
    ui/game_ui/style_resolver.cpp
    ui/game_ui/ui_layout.cpp
    ui/game_ui/ui_renderer.cpp
    ui/game_ui/ui_input.cpp
    ui/game_ui/ui_data_bind.cpp
    ui/game_ui/ui_animation.cpp
    ui/game_ui/ui_watcher.cpp
    ui/game_ui/widget_resolver.cpp
    ui/game_ui/ui_subsystem.cpp
    ui/game_ui/ui_widget_context.cpp
    ui/game_ui/ui_widget_component_meta.cpp
    ui/game_ui/widget_asset.cpp
    ui/game_ui/game_ui.cpp
    ui/game_ui/font_atlas.cpp
    ui/game_ui/text_shaper.cpp

    # ImGui sources
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp

    # ImGuizmo
    ${imguizmo_SOURCE_DIR}/ImGuizmo.cpp

    # imnodes
    ${imnodes_SOURCE_DIR}/imnodes.cpp

    # imgui-node-editor
    ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor.cpp
    ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor_api.cpp
    ${imgui_node_editor_SOURCE_DIR}/imgui_canvas.cpp
    ${imgui_node_editor_SOURCE_DIR}/crude_json.cpp
)

target_include_directories(fjell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/scripting
    ${stb_SOURCE_DIR}
)

target_include_directories(fjell SYSTEM PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imguizmo_SOURCE_DIR}
    ${imnodes_SOURCE_DIR}
    ${imgui_node_editor_SOURCE_DIR}
    ${miniaudio_SOURCE_DIR}
)

target_link_libraries(fjell PRIVATE
    fjell-platform
    fastgltf::fastgltf
    glfw
    glm::glm
    GPUOpen::VulkanMemoryAllocator
    Jolt
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Vulkan::Vulkan
    yogacore
    msdfgen::msdfgen-core
    meshoptimizer
    ozz_animation
    ozz_animation_offline
    msdfgen::msdfgen-ext
    Freetype::Freetype
    ${CMAKE_DL_LIBS}
)

# Tracy profiler — link and suppress warnings when enabled
if(FJELL_ENABLE_TRACY)
    target_link_libraries(fjell PRIVATE TracyClient)
    target_compile_definitions(fjell PRIVATE FJELL_ENABLE_TRACY)
    get_target_property(TRACY_INCLUDE_DIRS TracyClient INTERFACE_INCLUDE_DIRECTORIES)
    if(TRACY_INCLUDE_DIRS)
        target_include_directories(fjell SYSTEM PRIVATE ${TRACY_INCLUDE_DIRS})
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(fjell PRIVATE -rdynamic)
endif()

# Export symbols from the engine exe so game script DLLs can link against them.
# On Linux this is handled by -rdynamic above.
# On Windows, ENABLE_EXPORTS generates fjell.lib import library; symbols are
# marked for export via FJELL_SDK_EXPORT in the SDK headers.
set_target_properties(fjell PROPERTIES ENABLE_EXPORTS TRUE)

if(MSVC)
    target_compile_options(fjell PRIVATE /W4 /WX)
    target_compile_definitions(fjell PRIVATE _CRT_SECURE_NO_WARNINGS FJELL_ENGINE_BUILD)
    # Pass the full path to cl.exe so fjreflect can use it at runtime
    string(REPLACE "\\" "/" _FJELL_CXX_COMPILER "${CMAKE_CXX_COMPILER}")
    target_compile_definitions(fjell PRIVATE "FJELL_CXX_COMPILER=\"${_FJELL_CXX_COMPILER}\"")
    # Suppress warnings in third-party ImGuizmo source
    set_source_files_properties(${imguizmo_SOURCE_DIR}/ImGuizmo.cpp PROPERTIES COMPILE_FLAGS "/W0")
else()
    target_compile_options(fjell PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Treat VMA headers as system headers to suppress warnings from them
get_target_property(VMA_INCLUDE_DIRS GPUOpen::VulkanMemoryAllocator INTERFACE_INCLUDE_DIRECTORIES)
if(VMA_INCLUDE_DIRS)
    target_include_directories(fjell SYSTEM PRIVATE ${VMA_INCLUDE_DIRS})
endif()
# VMA implementation compiles third-party code — suppress warnings
if(MSVC)
    set_source_files_properties(renderer/gpu/vma_impl.cpp PROPERTIES COMPILE_FLAGS "/w")
else()
    set_source_files_properties(renderer/gpu/vma_impl.cpp PROPERTIES COMPILE_FLAGS "-w")
endif()

# imgui-node-editor — suppress warnings from third-party code
if(MSVC)
    set_source_files_properties(
        ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor.cpp
        ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor_api.cpp
        ${imgui_node_editor_SOURCE_DIR}/imgui_canvas.cpp
        ${imgui_node_editor_SOURCE_DIR}/crude_json.cpp
        PROPERTIES COMPILE_FLAGS "/w")
else()
    set_source_files_properties(
        ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor.cpp
        ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor_api.cpp
        ${imgui_node_editor_SOURCE_DIR}/imgui_canvas.cpp
        ${imgui_node_editor_SOURCE_DIR}/crude_json.cpp
        PROPERTIES COMPILE_FLAGS "-w")
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

# Treat msdfgen headers as system headers to suppress warnings from them
get_target_property(MSDFGEN_CORE_DIRS msdfgen-core INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(fjell SYSTEM PRIVATE ${MSDFGEN_CORE_DIRS})
get_target_property(MSDFGEN_EXT_DIRS msdfgen-ext INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(fjell SYSTEM PRIVATE ${MSDFGEN_EXT_DIRS})

set(SHADER_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
compile_shaders(fjell ${SHADER_DIR} ${SHADER_OUTPUT_DIR})

# Copy shader includes and .fjsl files to build dir (needed at runtime for FJSL compilation)
file(GLOB FJSL_SHADER_FILES "${SHADER_DIR}/*.fjsl")
file(GLOB SHADER_INCLUDE_FILES "${SHADER_DIR}/include/*.glsl")
add_custom_target(fjell_shader_resources ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}/include"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}/generated"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FJSL_SHADER_FILES} "${SHADER_OUTPUT_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHADER_INCLUDE_FILES} "${SHADER_OUTPUT_DIR}/include/"
    COMMENT "Copying FJSL shaders and includes to build directory"
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
    renderer/gpu/vma_impl.cpp
    hub/hub_ui.cpp
    hub/recent_projects.cpp
    ui/file_browser.cpp
    core/log.cpp
    core/window.cpp
    ui/console.cpp
    renderer/gpu/device.cpp
    renderer/gpu/gpu_core.cpp
    renderer/gpu/swapchain.cpp
    renderer/gpu/image.cpp
    renderer/gpu/buffer.cpp
    ui/imgui_layer.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${imguizmo_SOURCE_DIR}/ImGuizmo.cpp
)

target_include_directories(fjell-hub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${stb_SOURCE_DIR}
)
target_include_directories(fjell-hub SYSTEM PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imguizmo_SOURCE_DIR}
)
target_link_libraries(fjell-hub PRIVATE
    fjell-platform
    glfw
    GPUOpen::VulkanMemoryAllocator
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Vulkan::Vulkan
)
if(MSVC)
    target_compile_options(fjell-hub PRIVATE /W4 /WX)
    target_compile_definitions(fjell-hub PRIVATE _CRT_SECURE_NO_WARNINGS)
    set_source_files_properties(${imguizmo_SOURCE_DIR}/ImGuizmo.cpp PROPERTIES COMPILE_FLAGS "/W0")
else()
    target_compile_options(fjell-hub PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
# VMA system headers for fjell-hub
if(VMA_INCLUDE_DIRS)
    target_include_directories(fjell-hub SYSTEM PRIVATE ${VMA_INCLUDE_DIRS})
endif()
# VMA implementation — suppress warnings in fjell-hub too
if(MSVC)
    set_source_files_properties(renderer/gpu/vma_impl.cpp PROPERTIES COMPILE_FLAGS "/w")
else()
    set_source_files_properties(renderer/gpu/vma_impl.cpp PROPERTIES COMPILE_FLAGS "-w")
endif()

# --- Install rules ---
install(TARGETS fjell fjell-hub RUNTIME DESTINATION .)
install(DIRECTORY "${SHADER_OUTPUT_DIR}/" DESTINATION shaders
        FILES_MATCHING PATTERN "*.spv")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/assets/" DESTINATION assets)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/engine_assets/" DESTINATION engine_assets)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/scripting/fjell" DESTINATION sdk)
