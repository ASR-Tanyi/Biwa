// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/cli/run.h"

#include <array>
#include <filesystem>
#include <format>
#include <string>

#include "biwa/core/version.h"
#include "biwa/nx/pfs0.h"
#include "biwa/nx/storage.h"

namespace biwa::cli {
namespace {

using Args = std::span<const std::string_view>;
using CommandFn = int (*)(Args args, std::ostream& out, std::ostream& err);

struct Command {
    std::string_view name;
    std::string_view usage;    // arguments after the command name
    std::string_view summary;  // one line for `biwa help`
    CommandFn run;
};

std::string PathText(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

// `biwa <command>: <message>`, then the exit code for the error's category.
int Report(std::string_view command, const Error& error, std::ostream& err) {
    err << "biwa " << command << ": " << error.message() << '\n';
    return ExitCodeFor(error.code());
}

int UsageError(const Command& command, std::string_view problem, std::ostream& err) {
    err << "biwa " << command.name << ": " << problem << "\nusage: biwa " << command.name;
    if (!command.usage.empty()) {
        err << ' ' << command.usage;
    }
    err << '\n';
    return kExitUsage;
}

int RunVersion(Args args, std::ostream& out, std::ostream& err);
int RunHelp(Args args, std::ostream& out, std::ostream& err);
int RunInfo(Args args, std::ostream& out, std::ostream& err);

constexpr std::array kCommands = {
    Command{"info", "<file>", "list what is inside a Switch file (NSP)", RunInfo},
    Command{"version", "", "print Biwa's version", RunVersion},
    Command{"help", "", "print this text", RunHelp},
};

const Command& CommandNamed(std::string_view name) {
    for (const Command& command : kCommands) {
        if (command.name == name) {
            return command;
        }
    }
    return kCommands.back();  // only reached for names that exist
}

void PrintUsage(std::ostream& os) {
    os << "usage: biwa <command> [arguments]\n\ncommands:\n";
    for (const Command& command : kCommands) {
        const std::string left = std::format("{} {}", command.name, command.usage);
        os << std::format("  {:<16} {}\n", left, command.summary);
    }
}

int RunVersion(Args args, std::ostream& out, std::ostream& err) {
    if (!args.empty()) {
        return UsageError(CommandNamed("version"), std::format("unexpected argument '{}'", args[0]), err);
    }
    out << "biwa " << Version() << " (" << SourceCommit() << ")\n";
    return kExitOk;
}

int RunHelp(Args args, std::ostream& out, std::ostream& err) {
    if (!args.empty()) {
        return UsageError(CommandNamed("help"), std::format("unexpected argument '{}'", args[0]), err);
    }
    PrintUsage(out);
    return kExitOk;
}

int RunInfo(Args args, std::ostream& out, std::ostream& err) {
    if (args.size() != 1) {
        return UsageError(CommandNamed("info"), args.empty() ? "a file is needed" : "only one file at a time", err);
    }
    const std::filesystem::path path(args[0]);
    Result<nx::StoragePtr> storage = nx::OpenFileStorage(path);
    if (!storage) {
        return Report("info", storage.error(), err);
    }
    Result<nx::Pfs0> pfs = nx::Pfs0::Open(*storage);
    if (!pfs) {
        return Report("info", std::move(pfs).error().WithContext(PathText(path)), err);
    }
    std::uint64_t total = 0;
    out << PathText(path) << "\n  PFS0, " << pfs->entries().size() << (pfs->entries().size() == 1 ? " file" : " files")
        << '\n';
    for (const nx::Pfs0Entry& entry : pfs->entries()) {
        out << std::format("  {:>14}  {}\n", entry.size, entry.name);
        total += entry.size;
    }
    out << std::format("  {:>14}  bytes in all\n", total);
    return kExitOk;
}

}  // namespace

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
