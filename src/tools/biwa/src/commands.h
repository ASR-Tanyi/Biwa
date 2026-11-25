// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <filesystem>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

#include "biwa/core/error.h"

// Shared by the biwa commands; not part of the cli library's interface.
namespace biwa::cli {

using Args = std::span<const std::string_view>;

std::string PathText(const std::filesystem::path& path);

// Prints `biwa <command>: <message>` and returns the exit code for the error's category.
int Report(std::string_view command, const Error& error, std::ostream& err);

// Prints the problem and the command's usage line; returns the usage exit code.
int UsageError(std::string_view command, std::string_view usage, std::string_view problem, std::ostream& err);

int RunInfo(Args args, std::ostream& out, std::ostream& err);

}  // namespace biwa::cli
