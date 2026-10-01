# Mirror SHADER_SRC tree into SHADER_DST, copying every .fjsl (recursively,
# so lib/ comes with them) and every .glsl under include/. Runs at build time
# so additions don't require re-running cmake.

file(MAKE_DIRECTORY "${SHADER_DST}/include")
file(MAKE_DIRECTORY "${SHADER_DST}/generated")

# Recursive because a `use` names a path, and the path it names has to exist
# here: the engine resolves a composed function exactly the way it resolves
# the shader that uses it. `generated/` is this directory's own output and is
# skipped, or a generated .fjsl would be mirrored back over itself.
file(GLOB_RECURSE FJSL_FILES RELATIVE "${SHADER_SRC}" "${SHADER_SRC}/*.fjsl")
foreach(REL ${FJSL_FILES})
    if(REL MATCHES "^generated/")
        continue()
    endif()
    configure_file("${SHADER_SRC}/${REL}" "${SHADER_DST}/${REL}" COPYONLY)
endforeach()

# Remove .fjsl files the source tree no longer has. A mirror that only ever
# copies in keeps whatever was there before, and the engine resolves shader
# paths here: a stray takes precedence over a game's shader of the same name
# and silently renders the wrong one. Three of them sat here for five months.
file(GLOB_RECURSE MIRRORED_FJSL RELATIVE "${SHADER_DST}" "${SHADER_DST}/*.fjsl")
foreach(REL ${MIRRORED_FJSL})
    if(REL MATCHES "^generated/")
        continue()
    endif()
    if(NOT EXISTS "${SHADER_SRC}/${REL}")
        message(STATUS "Removing stale mirrored shader: ${REL}")
        file(REMOVE "${SHADER_DST}/${REL}")
    endif()
endforeach()

file(GLOB_RECURSE GLSL_INCLUDES RELATIVE "${SHADER_SRC}/include" "${SHADER_SRC}/include/*.glsl")
foreach(REL ${GLSL_INCLUDES})
    configure_file("${SHADER_SRC}/include/${REL}" "${SHADER_DST}/include/${REL}" COPYONLY)
endforeach()

# ftk's shader includes (the colour space) join the engine's: the FJSL
# templates include them when shaders are compiled at run time.
if(FTK_SHADER_INCLUDES)
    file(GLOB_RECURSE FTK_GLSL_INCLUDES RELATIVE "${FTK_SHADER_INCLUDES}" "${FTK_SHADER_INCLUDES}/*.glsl")
    foreach(REL ${FTK_GLSL_INCLUDES})
        configure_file("${FTK_SHADER_INCLUDES}/${REL}" "${SHADER_DST}/include/${REL}" COPYONLY)
    endforeach()
endif()
