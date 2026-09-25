add_executable(fjreflect
    main.cpp
    lexer.cpp
    parser.cpp
    generator.cpp
)

target_compile_features(fjreflect PRIVATE cxx_std_23)
if(MSVC)
    target_compile_options(fjreflect PRIVATE /W4 /WX)
else()
    target_compile_options(fjreflect PRIVATE -Wall -Wextra -Wpedantic ${FJELL_SHADOW_FLAG})
endif()

# Installed where ProjectManager looks for it, mirroring the dev build tree.
install(TARGETS fjreflect RUNTIME DESTINATION tools/fjreflect)
