// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/cli/run.h"

#include "biwa/core/version.h"

namespace biwa::cli {
namespace {

constexpr std::string_view kUsage =
    "usage: biwa <command> [options]\n"
    "\n"
    "commands:\n"
    "  version   print Biwa's version\n"
    "  help      print this text\n";

// Commands that take no arguments of their own.
bool NoExtraArguments(std::span<const std::string_view> args, std::ostream& err) {
    if (args.size() <= 1) {
        return true;
    }
    err << "biwa " << args[0] << ": unexpected argument '" << args[1] << "'\n";
    return false;
}

}  // namespace

int Run(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
    if (args.empty()) {
        err << kUsage;
        return kExitUsage;
    }
    const std::string_view command = args[0];
    if (command == "version" || command == "--version") {
        if (!NoExtraArguments(args, err)) {
            return kExitUsage;
        }
        out << "biwa " << Version() << " (" << SourceCommit() << ")\n";
        return kExitOk;
    }
    if (command == "help" || command == "--help" || command == "-h") {
        if (!NoExtraArguments(args, err)) {
            return kExitUsage;
        }
        out << kUsage;
        return kExitOk;
    }
    err << "biwa: unknown command '" << command << "'\n\n" << kUsage;
    return kExitUsage;
}

}  // namespace biwa::cli
