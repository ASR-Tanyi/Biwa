# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Biwa contributors

# Every third-party project is pinned by release and SHA-256 (docs/design/02-dependencies.md).
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

if(BIWA_BUILD_TESTS)
    # GoogleTest 1.18.0
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
        URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
        EXCLUDE_FROM_ALL
        SYSTEM)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    include(GoogleTest)
endif()
