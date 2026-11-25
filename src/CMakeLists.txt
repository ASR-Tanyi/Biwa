# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Biwa contributors

# Modules in layer order (docs/design/01-build-and-toolchain.md, "Module graph").
add_subdirectory(core)
add_subdirectory(nx)
add_subdirectory(a64)
add_subdirectory(tools/biwa)
