# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Biwa contributors

# biwa_add_test(<name> SOURCES <files...> DEPS <modules...>)
# A GoogleTest executable linked with the named Biwa modules. Tests are listed when ctest
# runs (PRE_TEST), so cross builds do not run Wine while building.
function(biwa_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;DEPS")
    add_executable(${name} ${ARG_SOURCES})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})  # "support/..."
    list(TRANSFORM ARG_DEPS PREPEND biwa::)
    target_link_libraries(${name} PRIVATE ${ARG_DEPS} GTest::gtest_main)
    biwa_warnings(${name})
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 120)  # Wine can start slowly
endfunction()

biwa_add_test(core_test
    SOURCES core/error_test.cpp core/file_test.cpp core/json_test.cpp core/log_test.cpp core/sha256_test.cpp
            core/version_test.cpp
    DEPS core)
target_compile_definitions(core_test PRIVATE BIWA_TEST_PROJECT_VERSION="${PROJECT_VERSION}")

biwa_add_test(cli_test SOURCES cli/run_test.cpp DEPS cli)

biwa_add_test(nx_test
    SOURCES nx/content_test.cpp nx/crypto_test.cpp nx/hash_test.cpp nx/keys_test.cpp nx/nca_test.cpp nx/ticket_test.cpp nx/pfs0_test.cpp nx/romfs_test.cpp nx/section_test.cpp nx/storage_test.cpp
    DEPS nx)
