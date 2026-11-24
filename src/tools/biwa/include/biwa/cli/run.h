// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace biwa::cli {

// Exit codes shared by every biwa command. Commands add their own above these
// (docs/design/03-extract.md).
inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 1;
inline constexpr int kExitInternal = 70;  // a bug in Biwa (an exception reached main)

// Runs the biwa command line. `args` excludes the program name. Normal output goes to `out`,
// errors and usage text to `err`. Returns the process exit code.
int Run(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace biwa::cli
