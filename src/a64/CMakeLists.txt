# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Biwa contributors

# A64 instruction decoder (docs/design/04-cpu-recompiler.md, "Decoder").

# The decoder table comes from dynarmic's a64.inc (third_party/dynarmic). Its INST(...) lines are
# turned into BIWA_A64_OP(id, "name", "pattern") lines at configure time; a name that repeats gets
# a number (UnallocatedEncoding_2, ...) so every entry can be an enumerator.
set(biwa_a64_inc ${PROJECT_SOURCE_DIR}/third_party/dynarmic/a64.inc)
set(biwa_a64_extra ${PROJECT_SOURCE_DIR}/third_party/dynarmic/a64_extra.inc)  # ARMv8.0 entries a64.inc leaves out
set(biwa_a64_ops ${CMAKE_CURRENT_BINARY_DIR}/generated/biwa/a64/ops.inc)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${biwa_a64_inc} ${biwa_a64_extra})
file(STRINGS ${biwa_a64_inc} biwa_a64_lines REGEX "^INST\\(")
file(STRINGS ${biwa_a64_extra} biwa_a64_extra_lines REGEX "^INST\\(")
list(APPEND biwa_a64_lines ${biwa_a64_extra_lines})
set(biwa_a64_text "// Generated from third_party/dynarmic/a64.inc and a64_extra.inc by src/a64/CMakeLists.txt. Do not edit.\n")
foreach(line IN LISTS biwa_a64_lines)
    if(NOT line MATCHES "^INST\\(([A-Za-z0-9_]+), *(\"[^\"]*\"), *\"([01a-zA-Z-]+)\"\\)")
        message(FATAL_ERROR "a64.inc: cannot read: ${line}")
    endif()
    set(id ${CMAKE_MATCH_1})
    set(name ${CMAKE_MATCH_2})
    set(pattern ${CMAKE_MATCH_3})
    string(LENGTH "${pattern}" length)
    if(NOT length EQUAL 32)
        message(FATAL_ERROR "a64.inc: ${id}: the pattern is not 32 bits")
    endif()
    if(DEFINED biwa_a64_seen_${id})
        math(EXPR biwa_a64_seen_${id} "${biwa_a64_seen_${id}} + 1")
        set(id "${id}_${biwa_a64_seen_${id}}")
    else()
        set(biwa_a64_seen_${id} 1)
    endif()
    string(APPEND biwa_a64_text "BIWA_A64_OP(${id}, ${name}, \"${pattern}\")\n")
endforeach()
file(CONFIGURE OUTPUT ${biwa_a64_ops} CONTENT "${biwa_a64_text}" @ONLY)

biwa_add_module(a64 LAYER 1
    SOURCES src/decoder.cpp src/interpreter.cpp
    DEPS core)
target_include_directories(biwa_a64 PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
# The FP semantics rely on the host's sqrt and fma instructions, which set no errno.
target_compile_options(biwa_a64 PRIVATE -fno-math-errno)
