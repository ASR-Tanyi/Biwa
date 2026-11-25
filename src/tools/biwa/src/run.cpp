// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/cli/run.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <string>

#include "biwa/core/version.h"
#include "commands.h"

namespace biwa::cli {
namespace {

using CommandFn = int (*)(Args args, std::ostream& out, std::ostream& err);

struct Command {
    std::string_view name;
    std::string_view usage;    // arguments after the command name
    std::string_view summary;  // one line for `biwa help`
    CommandFn run;
};

int RunVersion(Args args, std::ostream& out, std::ostream& err);
int RunHelp(Args args, std::ostream& out, std::ostream& err);

constexpr std::array kCommands = {
    Command{"info", "[-k <prod.keys>] [--verify] <file>",
            "list what is inside an NSP; -k adds its NCAs, --verify checks their hashes", RunInfo},
    Command{"romfs", "ls|cat [-k <prod.keys>] <file> [path]",
            "list or print RomFS files, from an NSP or a RomFS image", RunRomfs},
    Command{"version", "", "print Biwa's version", RunVersion},
    Command{"help", "", "print this text", RunHelp},
};

void PrintUsage(std::ostream& os) {
    os << "usage: biwa <command> [arguments]\n\ncommands:\n";
    std::size_t width = 0;
    for (const Command& command : kCommands) {
        width = std::max(width, command.name.size() + 1 + command.usage.size());
    }
    for (const Command& command : kCommands) {
        const std::string left = std::format("{} {}", command.name, command.usage);
        os << std::format("  {:<{}}  {}\n", left, width, command.summary);
    }
}

int RunVersion(Args args, std::ostream& out, std::ostream& err) {
    if (!args.empty()) {
        return UsageError("version", "", std::format("unexpected argument '{}'", args[0]), err);
    }
    out << "biwa " << Version() << " (" << SourceCommit() << ")\n";
    return kExitOk;
}

int RunHelp(Args args, std::ostream& out, std::ostream& err) {
    if (!args.empty()) {
        return UsageError("help", "", std::format("unexpected argument '{}'", args[0]), err);
    }
    PrintUsage(out);
    return kExitOk;
}

}  // namespace

std::string PathText(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

int Report(std::string_view command, const Error& error, std::ostream& err) {
    err << "biwa " << command << ": " << error.message() << '\n';
    return ExitCodeFor(error.code());
}

int UsageError(std::string_view command, std::string_view usage, std::string_view problem, std::ostream& err) {
    err << "biwa " << command << ": " << problem << "\nusage: biwa " << command;
    if (!usage.empty()) {
        err << ' ' << usage;
    }
    err << '\n';
    return kExitUsage;
}

int ExitCodeFor(Errc code) {
    switch (code) {
        case Errc::kInvalidArgument:
        case Errc::kNotFound:
        case Errc::kIo:
            return kExitInput;
        case Errc::kCorrupt:
            return kExitDamaged;
        case Errc::kMissingKey:
            return kExitMissingKeys;
        case Errc::kUnsupported:
            return kExitUnsupported;
        case Errc::kInternal:
            return kExitInternal;
    }
    return kExitInternal;
}

int Run(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
    if (args.empty()) {
        PrintUsage(err);
        return kExitUsage;
    }
    std::string_view name = args[0];
    if (name == "--version") {
        name = "version";
    } else if (name == "--help" || name == "-h") {
        name = "help";
    }
    for (const Command& command : kCommands) {
        if (command.name == name) {
            return command.run(args.subspan(1), out, err);
        }
    }
    err << "biwa: unknown command '" << args[0] << "'\n\n";
    PrintUsage(err);
    return kExitUsage;
}

}  // namespace biwa::cli
