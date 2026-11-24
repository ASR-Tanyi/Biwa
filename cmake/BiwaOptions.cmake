# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Biwa contributors

option(BIWA_WERROR "Treat warnings in Biwa's own code as errors" ON)
set(BIWA_SANITIZE "" CACHE STRING "Sanitizers for every target, e.g. address,undefined or thread")

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
endfunction()
