# CMP_Core's patch step, run in its source directory (cmake/Dependencies.cmake).
# Its BC6H encoder keeps each mode's quantised endpoints in an array it never
# sets: a one-region mode writes only the first region's, and an endpoint
# transform that overflows stops partway, yet the fit check decompresses both
# regions, adding whatever the stack held there (once enough to overflow an
# int). What it computes from them is thrown away, so the array starts at zero
# and the encoder writes the same bytes. Running it twice changes nothing; a
# release whose text no longer matches stops the configure.

set(source "cmp_core/shaders/bc6_encode_kernel.cpp")
set(unset "    CGU_INT quantEndPoints[MAX_BC6H_MODES + 1][MAX_SUBSETS][MAX_END_POINTS][MAX_DIMENSION_BIG];  // endpoints to save for a given mode\n")
set(zeroed "    CGU_INT quantEndPoints[MAX_BC6H_MODES + 1][MAX_SUBSETS][MAX_END_POINTS][MAX_DIMENSION_BIG] = {};  // endpoints to save for a given mode\n")

file(READ "${source}" text)
string(FIND "${text}" "${zeroed}" already)
if(already EQUAL -1)
    string(FIND "${text}" "${unset}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "CMP_Core's ${source} no longer declares its BC6H endpoints as cmake/CmpCoreBc6Endpoints.cmake expects")
    endif()
    string(REPLACE "${unset}" "${zeroed}" text "${text}")
    file(WRITE "${source}" "${text}")
endif()
