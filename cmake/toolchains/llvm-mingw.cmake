# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Biwa contributors

# Clang from llvm-mingw, target x86_64-w64-mingw32 with UCRT (docs/design/01-build-and-toolchain.md).
# Used both on Windows (native build) and on Linux (cross build, tests run under Wine).
#
# The toolchain is taken from $BIWA_LLVM_MINGW/bin when that environment variable is set,
# otherwise from PATH. The pinned release is llvm-mingw 20261117 (Clang 23.1.6).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(BIWA_MINGW_TRIPLE x86_64-w64-mingw32)

set(biwa_mingw_hints)
if(DEFINED ENV{BIWA_LLVM_MINGW})
    list(APPEND biwa_mingw_hints "$ENV{BIWA_LLVM_MINGW}/bin")
endif()
find_program(CMAKE_C_COMPILER ${BIWA_MINGW_TRIPLE}-clang HINTS ${biwa_mingw_hints} REQUIRED)
find_program(CMAKE_CXX_COMPILER ${BIWA_MINGW_TRIPLE}-clang++ HINTS ${biwa_mingw_hints} REQUIRED)
find_program(CMAKE_RC_COMPILER ${BIWA_MINGW_TRIPLE}-windres HINTS ${biwa_mingw_hints})

# CPU baseline, CodeView debug info (PDB files beside each binary), and a self-contained
# executable: libc++, libunwind and winpthreads linked statically.
set(CMAKE_C_FLAGS_INIT "-march=x86-64-v3 -gcodeview")
set(CMAKE_CXX_FLAGS_INIT "-march=x86-64-v3 -gcodeview")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -Wl,--pdb=")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,--pdb=")

# Cross builds run their tests under Wine.
if(NOT CMAKE_HOST_WIN32)
    find_program(BIWA_WINE NAMES wine64 wine PATHS /usr/lib/wine REQUIRED)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${BIWA_WINE})
endif()
