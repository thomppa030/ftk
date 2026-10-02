# Builds a file into a target, for a source to include as an array's bytes:
#
#     ftk_embed_file(<target> <file>)
#
#     constexpr char PALETTE[] = {
#     #include "dark.json.inc"
#     };
#
# writes <file>'s bytes as character literals to <name>.inc, in a directory
# on <target>'s private include path, before <target> is built and again
# whenever <file> changes. Also run on its own by that build step (script
# mode below).

function(ftk_embed_file target file)
    get_filename_component(name "${file}" NAME)
    set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/embedded_files/${target}")
    set(output "${output_dir}/${name}.inc")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_dir}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${file}" "-DOUTPUT=${output}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        DEPENDS "${file}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        COMMENT "Embedding ${name}"
        VERBATIM)
    target_sources(${target} PRIVATE "${output}")
    target_include_directories(${target} PRIVATE "${output_dir}")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND DEFINED INPUT AND DEFINED OUTPUT)
    file(READ "${INPUT}" bytes HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "'\\\\x\\1'," bytes "${bytes}")
    file(WRITE "${OUTPUT}" "${bytes}\n")
endif()
