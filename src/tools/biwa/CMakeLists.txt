# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Biwa contributors

# The command line lives in a library so tests can run it without starting a process.
biwa_add_module(cli LAYER 5
    SOURCES src/info.cpp src/romfs.cpp src/run.cpp
    DEPS core nx)

add_executable(biwa src/main.cpp)
target_link_libraries(biwa PRIVATE biwa::cli)
biwa_warnings(biwa)
