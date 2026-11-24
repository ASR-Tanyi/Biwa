# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Biwa contributors

# Every Biwa module is a static library with a layer number. A module may depend
# only on modules of a lower layer, so the architecture in
# docs/design/01-build-and-toolchain.md ("Module graph") is checked at configure time.
#
#   biwa_add_module(<name> LAYER <n> SOURCES <files...> [DEPS <modules...>] [LIBS <targets...>])
#
# Creates biwa_<name> with the alias biwa::<name>. Public headers live in
# include/biwa/<name>/ beside the CMakeLists.txt, private sources in src/.
# DEPS names other Biwa modules (without the biwa:: prefix); LIBS names third-party targets.
function(biwa_add_module name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "LAYER" "SOURCES;DEPS;LIBS")
    if(NOT DEFINED ARG_LAYER)
        message(FATAL_ERROR "biwa_add_module(${name}): LAYER is required")
    endif()
    foreach(dep IN LISTS ARG_DEPS)
        get_property(dep_layer GLOBAL PROPERTY BIWA_LAYER_${dep})
        if("${dep_layer}" STREQUAL "")
            message(FATAL_ERROR "biwa_add_module(${name}): unknown module '${dep}' (add it first)")
        endif()
        if(NOT dep_layer LESS ARG_LAYER)
            message(FATAL_ERROR "biwa_add_module(${name}): layer ${ARG_LAYER} may not depend on "
                                "'${dep}' (layer ${dep_layer})")
        endif()
    endforeach()
    set_property(GLOBAL PROPERTY BIWA_LAYER_${name} ${ARG_LAYER})

    add_library(biwa_${name} STATIC ${ARG_SOURCES})
    add_library(biwa::${name} ALIAS biwa_${name})
    target_include_directories(biwa_${name}
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    list(TRANSFORM ARG_DEPS PREPEND biwa::)
    target_link_libraries(biwa_${name} PUBLIC ${ARG_DEPS} ${ARG_LIBS})
    biwa_warnings(biwa_${name})
endfunction()
