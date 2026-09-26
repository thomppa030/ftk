add_executable(fjreflect
    main.cpp
    lexer.cpp
    parser.cpp
    types.cpp
    linker.cpp
    generator.cpp
)

target_link_libraries(fjreflect PRIVATE nlohmann_json::nlohmann_json)
fjell_target_defaults(fjreflect)

# Installed where ProjectManager looks for it, mirroring the dev build tree.
install(TARGETS fjreflect RUNTIME DESTINATION tools/fjreflect)
