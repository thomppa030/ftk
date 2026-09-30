# The compile settings every first-party target shares, set on the target
# rather than for a directory so that a project building Fjell as a
# subdirectory gets them on Fjell's targets and can give its own the same.
#
#     add_executable(fjlint ...)
#     fjell_target_defaults(fjlint)
#
# Call it where the target's own compile options begin: options added after
# it land after these on the command line.

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
        # Each flag after -Wpedantic fails the build here where MSVC's /W4 /WX
        # fails it on Windows:
        # -Wshadow: a local or parameter hides another local, a parameter, a
        #   class member or a global (C4456-C4459).
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow
            -Werror)
    endif()
endfunction()
