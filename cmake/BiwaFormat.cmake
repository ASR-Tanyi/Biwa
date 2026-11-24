# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Biwa contributors

# biwa_format_check: fails if any source file is not formatted.
# biwa_format:       formats them in place.
# Both cover src/ and tests/; the file list is taken at configure time.
file(GLOB_RECURSE BIWA_FORMAT_FILES CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/*.h ${PROJECT_SOURCE_DIR}/src/*.c ${PROJECT_SOURCE_DIR}/src/*.cpp
    ${PROJECT_SOURCE_DIR}/tests/*.h ${PROJECT_SOURCE_DIR}/tests/*.c ${PROJECT_SOURCE_DIR}/tests/*.cpp)

execute_process(COMMAND ${BIWA_CLANG_FORMAT} --version
    OUTPUT_VARIABLE biwa_clang_format_version ERROR_QUIET RESULT_VARIABLE biwa_clang_format_result)
if(biwa_clang_format_result EQUAL 0 AND NOT biwa_clang_format_version MATCHES "version ${BIWA_LINT_LLVM_VERSION}")
    string(STRIP "${biwa_clang_format_version}" biwa_clang_format_version)
    message(WARNING "biwa_format: expected clang-format ${BIWA_LINT_LLVM_VERSION}, found: "
                    "${biwa_clang_format_version}. Other versions may format differently.")
endif()

add_custom_target(biwa_format_check
    COMMAND ${BIWA_CLANG_FORMAT} --dry-run --Werror ${BIWA_FORMAT_FILES}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Checking formatting"
    VERBATIM)

add_custom_target(biwa_format
    COMMAND ${BIWA_CLANG_FORMAT} -i ${BIWA_FORMAT_FILES}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Formatting sources"
    VERBATIM)
