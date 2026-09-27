# Refuses to configure when engine code opens a namespace the script SDK uses.
#
# The SDK's systems are namespaces in fjell (fjell::scene, fjell::audio, ...)
# whose functions are defined in its headers, and the game library and the
# engine are one program when the game runs. An engine function with the name
# and signature of an SDK one, in the same namespace, is a second definition
# of the same function: which of the two a call reaches is up to the compiler
# and the dynamic linker, and a source that sees both can call itself. The
# engine names its own namespaces differently instead.

function(fjell_check_sdk_namespaces)
    # CONFIGURE_DEPENDS makes a new file re-run the check on the next build,
    # but script mode does not accept it.
    set(glob_flags "")
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set(glob_flags CONFIGURE_DEPENDS)
    endif()

    set(sdk_dir "${CMAKE_SOURCE_DIR}/src/scripting/fjell")
    file(GLOB sdk_headers ${glob_flags} "${sdk_dir}/*.hpp" "${sdk_dir}/*.h")
    set(sdk_namespaces "")
    foreach(header IN LISTS sdk_headers)
        file(STRINGS "${header}" opened REGEX "^namespace [a-z_]+ \\{")
        foreach(line IN LISTS opened)
            string(REGEX REPLACE "^namespace ([a-z_]+) \\{.*" "\\1" name "${line}")
            # fjell holds both halves, and every library has its own detail.
            if(NOT name STREQUAL "fjell" AND NOT name STREQUAL "detail")
                list(APPEND sdk_namespaces "${name}")
            endif()
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES sdk_namespaces)
    if(NOT sdk_namespaces)
        return()
    endif()

    list(JOIN sdk_namespaces "|" alternatives)
    set(pattern "^[ \t]*namespace[ \t]+(fjell::)?(${alternatives})[ \t]*\\{")

    set(offenders "")
    foreach(root IN LISTS ARGN)
        file(GLOB_RECURSE sources ${glob_flags}
            "${root}/*.cpp" "${root}/*.hpp" "${root}/*.h")
        foreach(source IN LISTS sources)
            if(source MATCHES "/src/scripting/fjell/")
                continue()
            endif()
            file(STRINGS "${source}" hits REGEX "${pattern}")
            foreach(hit IN LISTS hits)
                string(STRIP "${hit}" hit)
                file(RELATIVE_PATH rel "${CMAKE_SOURCE_DIR}" "${source}")
                list(APPEND offenders "  ${rel}: ${hit}")
            endforeach()
        endforeach()
    endforeach()

    if(offenders)
        list(JOIN offenders "\n" offenders)
        message(FATAL_ERROR
            "Engine code opens a namespace the script SDK uses (src/scripting/fjell/):\n"
            "${offenders}\n"
            "Give the engine's namespace another name.")
    endif()
endfunction()

# Script mode, for checking a tree without configuring:
#   cmake -DFJELL_SDK_NAMESPACE_ROOTS="src;tests" -P cmake/SdkNamespaceCheck.cmake
if(CMAKE_SCRIPT_MODE_FILE AND FJELL_SDK_NAMESPACE_ROOTS)
    set(CMAKE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
    fjell_check_sdk_namespaces(${FJELL_SDK_NAMESPACE_ROOTS})
endif()
