// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

// `biwa info [-k <keys>] [--verify] <file>`: what is inside an NSP. With keys, each NCA's header,
// its sections and the files of its PFS0 sections; with --verify, every section is read in full
// and checked against its hashes.

#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "biwa/cli/run.h"
#include "biwa/core/hex.h"
#include "biwa/nx/content.h"
#include "biwa/nx/keys.h"
#include "biwa/nx/nca.h"
#include "biwa/nx/pfs0.h"
#include "biwa/nx/storage.h"
#include "commands.h"

namespace biwa::cli {
namespace {

constexpr std::string_view kUsage = "[-k <prod.keys>] [--verify] <file>";

// Keeps the first error seen, so the exit code reflects it after everything was printed.
struct FirstError {
    std::optional<Errc> code;
    void Note(const Error& error) {
        if (!code) {
            code = error.code();
        }
    }
};

void ListPfs0(const nx::Pfs0& pfs, std::string_view indent, std::ostream& out) {
    for (const nx::Pfs0Entry& entry : pfs.entries()) {
        out << std::format("{}{:>14}  {}\n", indent, entry.size, entry.name);
    }
}

std::string SectionLine(const nx::NcaSection& section) {
    std::string line =
        std::format("section {}: {}, {}, {}, {} bytes", section.index, nx::FsTypeName(section.fs_type),
                    nx::HashTypeName(section.hash_type), nx::EncryptionName(section.encryption), section.size);
    if (section.HasPatch()) {
        line += ", patch";
    }
    if (section.HasSparse()) {
        line += ", sparse";
    }
    if (section.HasCompression()) {
        line += ", compressed";
    }
    return line;
}

// Reads all of `data`; each read checks the hashes of what it read.
Status ReadThrough(const nx::Storage& data) {
    return nx::ForEachPiece(data, [](std::uint64_t, std::span<const std::byte>) -> Status { return {}; });
}

void DescribeNca(const nx::Pfs0& nsp, const nx::Pfs0Entry& entry, const nx::KeySet& keys, const nx::TicketMap& tickets,
                 bool verify, std::ostream& out, FirstError& first) {
    auto fail = [&](const Error& error, std::string_view indent) {
        out << indent << "error: " << error << '\n';
        first.Note(error);
    };
    out << '\n' << "  " << entry.name << '\n';
    Result<nx::StoragePtr> storage = nsp.OpenEntry(entry);
    if (!storage) {
        return fail(storage.error(), "    ");
    }
    const Result<nx::NcaHeader> header = nx::ReadNcaHeader(**storage, keys);
    if (!header) {
        return fail(header.error(), "    ");
    }
    out << std::format("    {}, title {:016X}, key generation {}, {}\n", nx::ContentTypeName(header->content_type),
                       header->program_id, header->key_generation,
                       header->HasRightsId() ? "rights ID " + ToHex(header->rights_id) : std::string("no rights ID"));

    for (const nx::NcaSection& section : header->sections) {
        out << "    " << SectionLine(section) << '\n';
        const bool list = section.fs_type == nx::NcaFsType::kPartitionFs;
        if (!list && !verify) {
            continue;
        }
        Result<nx::StoragePtr> data = nx::OpenSectionFs(*storage, *header, section, keys, tickets);
        if (!data) {
            fail(data.error(), "      ");
            continue;
        }
        if (list) {
            const Result<nx::Pfs0> pfs = nx::Pfs0::Open(*data);
            if (!pfs) {
                fail(pfs.error(), "      ");
                continue;
            }
            ListPfs0(*pfs, "      ", out);
        }
        if (verify) {
            const Status read = ReadThrough(**data);
            if (read) {
                out << "      hashes match\n";
            } else {
                fail(read.error(), "      ");
            }
        }
    }
}

}  // namespace

int RunInfo(Args args, std::ostream& out, std::ostream& err) {
    std::optional<std::string_view> keys_path;
    std::optional<std::string_view> file;
    bool verify = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--verify") {
            verify = true;
        } else if (args[i] == "-k" || args[i] == "--keys") {
            if (i + 1 >= args.size()) {
                return UsageError("info", kUsage, "-k needs a key file", err);
            }
            keys_path = args[++i];
        } else if (!file) {
            file = args[i];
        } else {
            return UsageError("info", kUsage, "only one file at a time", err);
        }
    }
    if (!file) {
        return UsageError("info", kUsage, "a file is needed", err);
    }
    if (verify && !keys_path) {
        return UsageError("info", kUsage, "--verify needs -k", err);
    }

    std::optional<nx::KeySet> keys;
    if (keys_path) {
        Result<nx::KeySet> loaded = nx::KeySet::Load(std::filesystem::path(*keys_path));
        if (!loaded) {
            return Report("info", loaded.error(), err);
        }
        keys = std::move(*loaded);
    }

    const std::filesystem::path path(*file);
    Result<nx::StoragePtr> storage = nx::OpenFileStorage(path);
    if (!storage) {
        return Report("info", storage.error(), err);
    }
    Result<nx::Pfs0> nsp = nx::Pfs0::Open(*storage);
    if (!nsp) {
        return Report("info", std::move(nsp).error().WithContext(PathText(path)), err);
    }
    std::uint64_t total = 0;
    out << PathText(path) << "\n  PFS0, " << nsp->entries().size() << (nsp->entries().size() == 1 ? " file" : " files")
        << '\n';
    ListPfs0(*nsp, "  ", out);
    for (const nx::Pfs0Entry& entry : nsp->entries()) {
        total += entry.size;
    }
    out << std::format("  {:>14}  bytes in all\n", total);
    if (!keys) {
        return kExitOk;
    }

    FirstError first;
    const nx::TicketMap tickets = nx::ReadTickets(*nsp, [&](std::string_view name, const Error& error) {
        out << "  " << name << ": error: " << error << '\n';
        first.Note(error);
    });
    for (const nx::Pfs0Entry& entry : nsp->entries()) {
        if (entry.name.ends_with(".nca")) {
            DescribeNca(*nsp, entry, *keys, tickets, verify, out, first);
        }
    }
    return first.code ? ExitCodeFor(*first.code) : kExitOk;
}

}  // namespace biwa::cli
