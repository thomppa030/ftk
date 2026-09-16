# Mirror SHADER_SRC tree into SHADER_DST, copying .fjsl files at the top
# level and every .glsl under include/ (recursively). Runs at build time
# so additions don't require re-running cmake.

file(MAKE_DIRECTORY "${SHADER_DST}/include")
file(MAKE_DIRECTORY "${SHADER_DST}/generated")

file(GLOB FJSL_FILES "${SHADER_SRC}/*.fjsl")
foreach(F ${FJSL_FILES})
    get_filename_component(NAME ${F} NAME)
    configure_file("${F}" "${SHADER_DST}/${NAME}" COPYONLY)
endforeach()

# Remove .fjsl files the source tree no longer has. A mirror that only ever
# copies in keeps whatever was there before, and the engine resolves shader
# paths here: a stray takes precedence over a game's shader of the same name
# and silently renders the wrong one. Three of them sat here for five months.
file(GLOB MIRRORED_FJSL "${SHADER_DST}/*.fjsl")
foreach(F ${MIRRORED_FJSL})
    get_filename_component(NAME ${F} NAME)
    if(NOT EXISTS "${SHADER_SRC}/${NAME}")
        message(STATUS "Removing stale mirrored shader: ${NAME}")
        file(REMOVE "${F}")
    endif()
endforeach()

file(GLOB_RECURSE GLSL_INCLUDES RELATIVE "${SHADER_SRC}/include" "${SHADER_SRC}/include/*.glsl")
foreach(REL ${GLSL_INCLUDES})
    configure_file("${SHADER_SRC}/include/${REL}" "${SHADER_DST}/include/${REL}" COPYONLY)
endforeach()
