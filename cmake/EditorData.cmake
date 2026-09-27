# The files an editor built on fjell-editor-shell reads at run time, staged
# beside its binary:
#
#     fjell_stage_editor_data(<target> [DESTINATION <dir>])
#
# compiles the ImGui layer's sRGB fragment stage and copies the theme's fonts
# into <dir> (default fjell) next to <target>'s binary, before <target> is
# built and again whenever either changes. The program hands the paths to its
# ImGui layer:
#
#     ImGuiLayerFiles{.fonts = exe_dir / "fjell" / "fonts",
#                     .srgb_fragment = exe_dir / "fjell" / "imgui.frag.spv"}
#
# Fjell's own editor and hub read them from the engine's layout instead.

function(fjell_stage_editor_data target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "DESTINATION" "")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "fjell_stage_editor_data(<target> [DESTINATION <dir>])")
    endif()
    set(destination fjell)
    if(arg_DESTINATION)
        set(destination "${arg_DESTINATION}")
    endif()

    fjell_find_glslc()
    get_filename_component(fjell_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(staging "${CMAKE_CURRENT_BINARY_DIR}/${target}_editor_data")

    set(fragment "${fjell_root}/shaders/imgui.frag")
    set(spirv "${staging}/imgui.frag.spv")
    file(GLOB includes CONFIGURE_DEPENDS "${fjell_root}/shaders/include/*.glsl")
    add_custom_command(
        OUTPUT "${spirv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${staging}"
        COMMAND "${GLSLC}" --target-env=vulkan1.3 -I "${fjell_root}/shaders/include"
                "${fragment}" -o "${spirv}"
        DEPENDS "${fragment}" ${includes}
        COMMENT "Compiling ${target}'s editor shader: imgui.frag"
        VERBATIM)
    set(staged "${spirv}")

    file(GLOB fonts CONFIGURE_DEPENDS "${fjell_root}/engine_assets/fonts/*.ttf")
    foreach(font IN LISTS fonts)
        get_filename_component(name "${font}" NAME)
        add_custom_command(
            OUTPUT "${staging}/fonts/${name}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${font}" "${staging}/fonts/${name}"
            DEPENDS "${font}"
            VERBATIM)
        list(APPEND staged "${staging}/fonts/${name}")
    endforeach()

    # Copied as a directory beside the binary, whose place is only known per
    # configuration, so the copy runs with every build and changes nothing
    # when nothing changed.
    add_custom_target(${target}-editor-data
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${staging}"
                "$<TARGET_FILE_DIR:${target}>/${destination}"
        DEPENDS ${staged}
        VERBATIM)
    add_dependencies(${target} ${target}-editor-data)
endfunction()
