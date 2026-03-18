find_program(GLSLC glslc REQUIRED)

function(compile_shaders TARGET SHADER_DIR OUTPUT_DIR)
    file(GLOB_RECURSE SHADERS
        "${SHADER_DIR}/*.vert"
        "${SHADER_DIR}/*.frag"
        "${SHADER_DIR}/*.comp"
        "${SHADER_DIR}/*.geom"
        "${SHADER_DIR}/*.tesc"
        "${SHADER_DIR}/*.tese"
        "${SHADER_DIR}/*.mesh"
        "${SHADER_DIR}/*.task"
    )

    foreach(SHADER ${SHADERS})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPIRV_OUTPUT "${OUTPUT_DIR}/${SHADER_NAME}.spv")

        add_custom_command(
            OUTPUT ${SPIRV_OUTPUT}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_DIR}
            COMMAND ${GLSLC} --target-env=vulkan1.3 ${SHADER} -o ${SPIRV_OUTPUT}
            DEPENDS ${SHADER}
            COMMENT "Compiling shader: ${SHADER_NAME}"
            VERBATIM
        )

        list(APPEND SPIRV_OUTPUTS ${SPIRV_OUTPUT})
    endforeach()

    add_custom_target(${TARGET}_shaders ALL DEPENDS ${SPIRV_OUTPUTS})
    add_dependencies(${TARGET} ${TARGET}_shaders)
endfunction()
