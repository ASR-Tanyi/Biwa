# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Biwa contributors

option(BIWA_WERROR "Treat warnings in Biwa's own code as errors" ON)
option(BIWA_UNICORN "Build the CPU tests that compare with Unicorn (Linux only; Unicorn is GPL-2.0 and test-only)" OFF)
set(BIWA_SANITIZE "" CACHE STRING "Sanitizers for every target, e.g. address,undefined or thread")
set(BIWA_CLANG_TIDY "" CACHE STRING "clang-tidy command run on Biwa's own targets (the lint preset sets it)")
set(BIWA_CLANG_FORMAT "clang-format" CACHE STRING "clang-format used by the biwa_format targets")
set(BIWA_LINT_LLVM_VERSION "23.1.4" CACHE INTERNAL "clang-format / clang-tidy version the style is checked with")

# Sanitizers apply globally, so dependencies are instrumented too.
if(BIWA_SANITIZE)
    add_compile_options(-fsanitize=${BIWA_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${BIWA_SANITIZE})
endif()

# Warnings for Biwa's own targets (never for third-party code).
function(biwa_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion -Wimplicit-fallthrough
        $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual>
        $<$<BOOL:${BIWA_WERROR}>:-Werror>)
    if(BIWA_CLANG_TIDY)
        set_target_properties(${target} PROPERTIES
            C_CLANG_TIDY "${BIWA_CLANG_TIDY}"
            CXX_CLANG_TIDY "${BIWA_CLANG_TIDY}")
    endif()
endfunction()
