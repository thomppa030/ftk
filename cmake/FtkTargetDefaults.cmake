# The compile settings every first-party target shares, set on the target
# rather than for a directory so that a project building ftk as a
# subdirectory gets them on ftk's targets and can give its own the same.
#
#     add_executable(fjlint ...)
#     ftk_target_defaults(fjlint)
#
# Call it where the target's own compile options begin: options added after
# it land after these on the command line.

function(ftk_target_defaults target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
        # A COFF object holds at most 65,279 sections, and every inline
        # function or template a source instantiates takes one: a source
        # that includes enough of the engine passes that (C1128). /bigobj
        # raises the limit; the code it compiles is the same.
        target_compile_options(${target} PRIVATE /bigobj)
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
        # -Wmismatched-tags: a type declared class in one place and struct in
        #   another (C4099).
        # -Wfloat-conversion: an implicit double-to-float or float-to-int
        #   conversion, the floating-point half of C4244.
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wmismatched-tags -Wfloat-conversion
            -Werror)
        # GCC's guess that a value may be read before it is set, which its
        # manual says misfires once optimisation inlines enough: GCC 16 at
        # -O3 takes the empty vertex attributes of a pipeline description
        # for unset in std::vector's destructor, however the description is
        # written. -Wuninitialized, for what is surely unset, stays on.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -Wno-maybe-uninitialized)
        endif()
    endif()
endfunction()

# Compiles <target> optimised in every configuration, Debug too: for code
# whose speed decides how usable a Debug build is, such as the texture
# encoders, which run several times slower unoptimised. MSVC's runtime
# checks do not combine with optimisation, so the target goes without them.
#
#     ftk_target_optimised(ftk-image)

function(ftk_target_optimised target)
    if(MSVC)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_CHECKS "")
        target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/O2>)
    else()
        target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:-O2>)
    endif()
endfunction()
