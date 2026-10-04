# CMP_Core's patch step, run in its source directory (cmake/Dependencies.cmake).
# Its BC7 encoder prints a line to standard output whenever one of its mode
# searches finds nothing better, which ordinary textures cause, and a library
# has no place writing to a program's output. The CPU build's ASPM_PRINT, its
# one way of printing, is made to print nothing. Running it twice changes
# nothing; a release whose text no longer matches stops the configure.

set(header "cmp_core/shaders/common_def.h")
set(printing "// using CPU compiler\n#define ASPM_PRINT(args) printf args\n")
set(quiet "// using CPU compiler\n#define ASPM_PRINT(args)\n")

file(READ "${header}" text)
string(FIND "${text}" "${quiet}" already)
if(already EQUAL -1)
    string(FIND "${text}" "${printing}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "CMP_Core's ${header} no longer defines ASPM_PRINT as cmake/CmpCoreQuiet.cmake expects")
    endif()
    string(REPLACE "${printing}" "${quiet}" text "${text}")
    file(WRITE "${header}" "${text}")
endif()
