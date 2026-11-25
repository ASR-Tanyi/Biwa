// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <ostream>
#include <span>
#include <string_view>

#include "biwa/core/error.h"

namespace biwa::cli {

// Exit codes shared by every biwa command (docs/design/03-extract.md).
inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 1;        // bad command line
inline constexpr int kExitInput = 2;        // input not found, unreadable or not a Switch file
inline constexpr int kExitDamaged = 3;      // damaged input: hash mismatch, bad tables, truncated
inline constexpr int kExitMissingKeys = 4;  // a needed key is not in the key files
inline constexpr int kExitUnsupported = 5;  // valid input Biwa does not handle yet
inline constexpr int kExitInternal = 70;    // a bug in Biwa

// The exit code for an error a command could not handle.
int ExitCodeFor(Errc code);

// Runs the biwa command line. `args` excludes the program name. Normal output goes to `out`,
// errors and usage text to `err`. Returns the process exit code.
int Run(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace biwa::cli
