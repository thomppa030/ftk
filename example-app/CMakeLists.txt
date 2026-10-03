# ftk-example-app, ftk's own test program: a window of pages, each showing one thing
# ftk's UI has to get right, so the toolkit is proven on its own before a
# program depends on it. Built with ftk's tests.

add_executable(ftk-example-app main.cpp kit_page.cpp painter.cpp primitives_page.cpp shell_page.cpp text_page.cpp)
target_link_libraries(ftk-example-app PRIVATE ftk::app ftk::app-ui ftk::image ftk::platform)
ftk_target_defaults(ftk-example-app)
ftk_stage_editor_data(ftk-example-app)

# The example app's own UI is the kit's, as any program's would be.
ftk_check_editor_ui(ftk-example-app ROOTS . ICONS icons.hpp)
