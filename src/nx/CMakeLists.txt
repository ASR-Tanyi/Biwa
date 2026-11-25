# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Biwa contributors

# Switch formats (docs/design/03-extract.md).
biwa_add_module(nx LAYER 1
    SOURCES
        src/keys.cpp
        src/pfs0.cpp
        src/storage.cpp
    DEPS core)
