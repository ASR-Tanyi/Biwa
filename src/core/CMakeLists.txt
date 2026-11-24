# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Biwa contributors

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE BIWA_SOURCE_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE biwa_git_result
    ERROR_QUIET)
if(NOT biwa_git_result EQUAL 0 OR BIWA_SOURCE_COMMIT STREQUAL "")
    set(BIWA_SOURCE_COMMIT unknown)
endif()
configure_file(src/version.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/gen/version.cpp @ONLY)

biwa_add_module(core LAYER 0
    SOURCES
        src/error.cpp
        src/file.cpp
        src/log.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/gen/version.cpp)
