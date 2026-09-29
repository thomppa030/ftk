# Finds glslc and keeps it in the cache as GLSLC. Called by whatever compiles
# a shader, so a program that links only libraries without shaders never needs
# it. Prefers the glslc that ships with the located Vulkan SDK: the Windows SDK
# installer leaves %VULKAN_SDK%\Bin off PATH, so a bare PATH search fails there
# even with the SDK correctly installed.
function(fjell_find_glslc)
    if(GLSLC)
        return()
    endif()
    if(Vulkan_GLSLC_EXECUTABLE)
        set(GLSLC "${Vulkan_GLSLC_EXECUTABLE}" CACHE FILEPATH "glslc shader compiler")
    else()
        find_program(GLSLC glslc
            HINTS ENV VULKAN_SDK
            PATH_SUFFIXES Bin bin)
    endif()
    if(NOT GLSLC)
        message(FATAL_ERROR
            "glslc not found. Install the Vulkan SDK (https://vulkan.lunarg.com/) and make "
            "sure VULKAN_SDK is set, or point -DGLSLC=<path> at the compiler directly.")
    endif()
endfunction()

function(compile_shaders TARGET SHADER_DIR OUTPUT_DIR)
    fjell_find_glslc()
    file(GLOB_RECURSE SHADERS
        "${SHADER_DIR}/*.vert"
        "${SHADER_DIR}/*.frag"
        "${SHADER_DIR}/*.comp"
        "${SHADER_DIR}/*.geom"
        "${SHADER_DIR}/*.tesc"
        "${SHADER_DIR}/*.tese"
        "${SHADER_DIR}/*.mesh"
        "${SHADER_DIR}/*.task"
        "${SHADER_DIR}/*.rgen"
        "${SHADER_DIR}/*.rchit"
        "${SHADER_DIR}/*.rmiss"
    )

    # Collect include files so shaders recompile when includes change
    file(GLOB_RECURSE SHADER_INCLUDES "${SHADER_DIR}/include/*.glsl")

    foreach(SHADER ${SHADERS})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPIRV_OUTPUT "${OUTPUT_DIR}/${SHADER_NAME}.spv")

        add_custom_command(
            OUTPUT ${SPIRV_OUTPUT}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_DIR}
            COMMAND ${GLSLC} --target-env=vulkan1.3
                    -I ${SHADER_DIR}/include
                    ${SHADER} -o ${SPIRV_OUTPUT}
            DEPENDS ${SHADER} ${SHADER_INCLUDES}
            COMMENT "Compiling shader: ${SHADER_NAME}"
            VERBATIM
        )

        list(APPEND SPIRV_OUTPUTS ${SPIRV_OUTPUT})
    endforeach()

    add_custom_target(${TARGET}_shaders ALL DEPENDS ${SPIRV_OUTPUTS})
    add_dependencies(${TARGET} ${TARGET}_shaders)
endfunction()

# Compiles one shader into its SPIR-V words as an initializer list
# (glslc -mfmt=num), which TARGET's sources include as "<name>.spv.inc": for
# a library's own shaders, which travel inside it rather than beside the
# program that links it.
function(fjell_embed_shader TARGET SHADER)
    fjell_find_glslc()
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders/${TARGET}")
    set(OUTPUT "${OUTPUT_DIR}/${SHADER_NAME}.spv.inc")
    add_custom_command(
        OUTPUT ${OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_DIR}
        COMMAND ${GLSLC} --target-env=vulkan1.3 -mfmt=num ${SHADER} -o ${OUTPUT}
        DEPENDS ${SHADER}
        COMMENT "Embedding shader: ${SHADER_NAME}"
        VERBATIM
    )
    target_sources(${TARGET} PRIVATE ${OUTPUT})
    target_include_directories(${TARGET} PRIVATE ${OUTPUT_DIR})
endfunction()
