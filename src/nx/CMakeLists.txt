# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Biwa contributors

# Switch formats (docs/design/03-extract.md).
biwa_add_module(nx LAYER 1
    SOURCES
        src/crypto.cpp
        src/hash.cpp
        src/keys.cpp
        src/nca.cpp
        src/pfs0.cpp
        src/section.cpp
        src/storage.cpp
        src/ticket.cpp
    DEPS core)
target_link_libraries(biwa_nx PRIVATE MbedTLS::mbedcrypto)
