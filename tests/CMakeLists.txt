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

biwa_add_test(a64_test
    SOURCES a64/capstone_test.cpp a64/decoder_test.cpp a64/interpreter_test.cpp a64/sem_c_test.c a64/sweep.cpp
    DEPS a64)
target_link_libraries(a64_test PRIVATE capstone_static)

if(BIWA_UNICORN)
    # The reference CPU harness and the tests that compare with it.
    biwa_add_test(a64_unicorn_test
        SOURCES a64/cpu_state.cpp a64/semantics_test.cpp a64/unicorn_cpu.cpp a64/unicorn_test.cpp
        DEPS a64)
    target_link_libraries(a64_unicorn_test PRIVATE unicorn)
endif()

# The full 2^32 comparison with Capstone; run by hand (it takes minutes).
add_executable(a64_sweep a64/sweep_main.cpp a64/sweep.cpp)
target_include_directories(a64_sweep PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(a64_sweep PRIVATE biwa::a64 capstone_static)
biwa_warnings(a64_sweep)

biwa_add_test(cli_test SOURCES cli/extract_test.cpp cli/run_test.cpp DEPS cli)
target_link_libraries(cli_test PRIVATE LZ4::lz4)  # the builders compress test data

biwa_add_test(nx_test
    SOURCES nx/bucket_tree_test.cpp nx/compressed_test.cpp nx/content_test.cpp nx/crypto_test.cpp
            nx/hash_test.cpp nx/keys_test.cpp nx/meta_test.cpp nx/nca_test.cpp nx/npdm_test.cpp
            nx/nso_test.cpp nx/patch_test.cpp nx/pfs0_test.cpp nx/romfs_test.cpp nx/section_test.cpp
            nx/storage_test.cpp nx/ticket_test.cpp
    DEPS nx)
target_link_libraries(nx_test PRIVATE LZ4::lz4)  # the builders compress test data
