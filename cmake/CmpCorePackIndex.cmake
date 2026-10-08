# CMP_Core's patch step, run in its source directory (cmake/Dependencies.cmake).
# Its BC7 encoder packs a block's sixteen 4-bit indices into two 32-bit words
# and never starts the shift again for the second, so indices 8 to 15 are
# shifted by 32 to 60 bits: undefined, and right only because x86 takes a
# 32-bit shift's count modulo 32. The shift starts again at the second word,
# as its unpack_index beside it already does, which packs the same bytes x86
# packed. Running it twice changes nothing; a release whose text no longer
# matches stops the configure.

set(source "cmp_core/shaders/bc7_encode_kernel.cpp")
set(overrun "        packed_index[k / 8] |= (CGV_UINT32)(src_index[k] & 0x0F) << shift;\n        shift += 4;\n")
set(restarted "        packed_index[k / 8] |= (CGV_UINT32)(src_index[k] & 0x0F) << shift;\n        if (k == 7)\n            shift = 0;\n        else\n            shift += 4;\n")

file(READ "${source}" text)
string(FIND "${text}" "${restarted}" already)
if(already EQUAL -1)
    string(FIND "${text}" "${overrun}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "CMP_Core's ${source} no longer packs BC7 indices as cmake/CmpCorePackIndex.cmake expects")
    endif()
    string(REPLACE "${overrun}" "${restarted}" text "${text}")
    file(WRITE "${source}" "${text}")
endif()
