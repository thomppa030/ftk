# The compile settings every first-party target shares, set on the target
# rather than for a directory so that a project building Fjell as a
# subdirectory gets them on Fjell's targets and can give its own the same.
#
#     add_executable(fjlint ...)
#     fjell_target_defaults(fjlint)
#
# Call it where the target's own compile options begin: options added after
# it land after these on the command line.

# MSVC warns when a local hides another local or a parameter (C4456/C4457)
# and /WX makes that a CI failure. GCC's -Wshadow=local is that same set;
# Clang has no =local form, and its plain -Wshadow covers the same cases.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(FJELL_SHADOW_FLAG -Wshadow=local)
else()
    set(FJELL_SHADOW_FLAG -Wshadow)
endif()

function(fjell_target_defaults target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
        # The engine reads and writes its binary formats with the C stdio
        # family, which MSVC deprecates in favour of the *_s variants under
        # /W4 /WX. Set on every first-party target, so a source shared
        # between the engine, the tools and the tests compiles the same way
        # in each. Only on our targets: enet defines the same macro while it
        # builds itself and warns about the redefinition when one reaches
        # its target from a directory.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic ${FJELL_SHADOW_FLAG} -Werror)
    endif()
endfunction()
