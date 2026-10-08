// The leaks a program's LeakSanitizer is not to report, read by the sanitizer
// runtime when the program is built with one and never called otherwise.
// ftk_leak_suppressions() (src/libraries.cmake) adds this file to a program.
//
// fontconfig 2.15, Ubuntu 24.04's, leaves memory LeakSanitizer reports as lost
// while it reads the system's font configuration, which ftk has it do once, in
// FcInitLoadConfigAndFonts (system_fonts_linux.cpp); 2.18 leaves none. Matched
// by that function alone, so a pattern or font set ftk forgets to destroy is
// still reported.

extern "C" const char* __lsan_default_suppressions();

extern "C" const char* __lsan_default_suppressions() {
    return "leak:FcInitLoadConfigAndFonts\n";
}
