# ftk-example-app, ftk's own test program: a window of pages, each showing one thing
# ftk's UI has to get right, so the toolkit is proven on its own before a
# program depends on it. Built with ftk's tests, twice from one source: on the
# UI core, and on the kit's ImGui implementation to compare against.

add_executable(ftk-example-app main.cpp kit_page.cpp painter.cpp primitives_page.cpp text_page.cpp)
target_link_libraries(ftk-example-app PRIVATE ftk::app ftk::app-ui ftk::image ftk::platform)
ftk_target_defaults(ftk-example-app)
ftk_stage_editor_data(ftk-example-app)

add_executable(ftk-example-app-imgui main.cpp)
target_compile_definitions(ftk-example-app-imgui PRIVATE FTK_EXAMPLE_APP_IMGUI)
target_link_libraries(ftk-example-app-imgui PRIVATE ftk::app-imgui ftk::image ftk::platform)
ftk_target_defaults(ftk-example-app-imgui)
ftk_stage_editor_data(ftk-example-app-imgui)

# The example app's own UI is the kit's, as any program's would be.
ftk_check_editor_ui(ftk-example-app ROOTS .)
ftk_check_imgui(ftk-example-app ROOTS .)
