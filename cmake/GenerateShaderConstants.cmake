# Reads renderer_constants.hpp + shadow_pass.hpp and emits a GLSL header
# mirroring the C++ constants the shaders need. Invoked at configure time
# by the parent CMakeLists.txt — the generated file lives in shaders/include/
# so it sits alongside the hand-written .glsl includes and gets picked up by
# MirrorShaderSources at build time.
#
# Only the constants enumerated below are emitted. If a shader needs a new
# C++ value, add its name to FJELL_SHADER_CONSTS and rerun cmake.

set(FJELL_SHADER_CONSTS
    MAX_LIGHTS_PER_TILE
    LIGHT_TILE_SIZE
    MAX_TILES
    MAX_PIPELINE_BINS
    LOCAL_SHADOW_ATLAS_SIZE
    LOCAL_SHADOW_TILE_SIZE
    MAX_SHADOWED_SPOT_LIGHTS
    MAX_SHADOWED_POINT_LIGHTS
)

# Sources to scan, in order. Later files override earlier ones if a name
# appears twice (allows specialising in a more authoritative header).
set(FJELL_CONST_SOURCES
    "${CMAKE_SOURCE_DIR}/src/renderer/renderer_constants.hpp"
    "${CMAKE_SOURCE_DIR}/src/renderer/pass/shadow_pass.hpp"
)

set(_const_kv "")
foreach(src ${FJELL_CONST_SOURCES})
    file(READ "${src}" _src_text)
    foreach(name ${FJELL_SHADER_CONSTS})
        if(_src_text MATCHES "constexpr[ \t]+uint32_t[ \t]+${name}[ \t]*=[ \t]*([0-9]+)")
            set(_const_kv_${name} ${CMAKE_MATCH_1})
        endif()
    endforeach()
endforeach()

# SHADOW_MAP_SIZE lives in ShadowPass as MAP_SIZE — pull it explicitly.
file(READ "${CMAKE_SOURCE_DIR}/src/renderer/pass/shadow_pass.hpp" _shadow_text)
if(_shadow_text MATCHES "constexpr[ \t]+uint32_t[ \t]+MAP_SIZE[ \t]*=[ \t]*([0-9]+)")
    set(_const_kv_SHADOW_MAP_SIZE ${CMAKE_MATCH_1})
endif()

# Build the generated file body.
set(_glsl_body "")
string(APPEND _glsl_body "#ifndef FJELL_RENDERER_CONSTANTS_GLSL\n")
string(APPEND _glsl_body "#define FJELL_RENDERER_CONSTANTS_GLSL\n\n")
string(APPEND _glsl_body "// Generated from C++ headers by cmake/GenerateShaderConstants.cmake.\n")
string(APPEND _glsl_body "// Do not edit by hand — update the source .hpp and rerun cmake.\n\n")

foreach(name ${FJELL_SHADER_CONSTS} SHADOW_MAP_SIZE)
    if(DEFINED _const_kv_${name})
        string(APPEND _glsl_body "const uint ${name} = ${_const_kv_${name}};\n")
    else()
        message(WARNING "GenerateShaderConstants: ${name} not found in C++ sources")
    endif()
endforeach()

# Provide a float alias for sizes that shaders divide by texel space.
string(APPEND _glsl_body "\n")
string(APPEND _glsl_body "const float LOCAL_SHADOW_ATLAS_SIZE_F = float(LOCAL_SHADOW_ATLAS_SIZE);\n")
string(APPEND _glsl_body "const float SHADOW_MAP_SIZE_F = float(SHADOW_MAP_SIZE);\n")
string(APPEND _glsl_body "\n#endif // FJELL_RENDERER_CONSTANTS_GLSL\n")

set(_out_path "${CMAKE_SOURCE_DIR}/shaders/include/fjell_renderer_constants.glsl")

# Write only if different — avoids touching mtimes and triggering rebuilds.
if(EXISTS "${_out_path}")
    file(READ "${_out_path}" _existing)
    if("${_existing}" STREQUAL "${_glsl_body}")
        return()
    endif()
endif()

file(WRITE "${_out_path}" "${_glsl_body}")
message(STATUS "Wrote ${_out_path}")
