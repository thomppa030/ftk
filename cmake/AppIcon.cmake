# A Windows program's icon, the one Explorer, the Start menu and the taskbar
# show: an SVG drawn into an ICO file while the program builds, and built into
# the program as its first icon resource. Other systems take a program's icon
# from its desktop entry or its window (ftk::set_app_identity), so there it
# does nothing.
#
#     ftk_app_icon(fjedit "${CMAKE_SOURCE_DIR}/assets/brand/fjedit-tile.svg")

# ftk-ico draws the SVG into the ICO file. Built only for a program that asks.
add_executable(ftk-ico EXCLUDE_FROM_ALL "${CMAKE_CURRENT_LIST_DIR}/../tools/ico.cpp")
target_link_libraries(ftk-ico PRIVATE ftk::base ftk::image)
ftk_target_defaults(ftk-ico)

function(ftk_app_icon target svg)
    if(NOT WIN32)
        return()
    endif()
    get_filename_component(svg "${svg}" ABSOLUTE)
    set(folder "${CMAKE_CURRENT_BINARY_DIR}/${target}_icon")
    set(ico "${folder}/${target}.ico")
    set(rc "${folder}/${target}.rc")
    add_custom_command(
        OUTPUT "${ico}"
        COMMAND ftk-ico "${svg}" "${ico}"
        DEPENDS ftk-ico "${svg}"
        COMMENT "Drawing ${target}'s icon"
        VERBATIM)
    # The resource compiler reads a backslash in a string as an escape, so the
    # path is written with forward slashes, which it takes as they are. The
    # file is written only when it says something new, so configuring again
    # doesn't build the resource again.
    file(TO_CMAKE_PATH "${ico}" ico_path)
    file(CONFIGURE OUTPUT "${rc}" CONTENT "1 ICON \"@ico_path@\"\n" @ONLY)
    set_source_files_properties("${rc}" PROPERTIES OBJECT_DEPENDS "${ico}")
    target_sources(${target} PRIVATE "${rc}" "${ico}")
endfunction()
