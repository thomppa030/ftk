add_executable(fjreflect
    main.cpp
    parser.cpp
    generator.cpp
)

target_compile_features(fjreflect PRIVATE cxx_std_23)
if(MSVC)
    target_compile_options(fjreflect PRIVATE /W4 /WX)
else()
    target_compile_options(fjreflect PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS fjreflect RUNTIME DESTINATION bin)
