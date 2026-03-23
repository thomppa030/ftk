add_executable(fjreflect
    main.cpp
    parser.cpp
    generator.cpp
)

target_compile_features(fjreflect PRIVATE cxx_std_23)
target_compile_options(fjreflect PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS fjreflect RUNTIME DESTINATION bin)
