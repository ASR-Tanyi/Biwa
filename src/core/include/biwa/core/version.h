// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <string_view>

namespace biwa {

// Biwa's version, from the top-level CMakeLists.txt (for example "0.0.1").
std::string_view Version();

// The commit the build was configured from (12 hex digits), or "unknown" outside a git checkout.
// Set at configure time, so it can lag behind until CMake runs again.
std::string_view SourceCommit();

}  // namespace biwa
