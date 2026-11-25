// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

// `biwa info [-k <keys>] <file>`: what is inside an NSP. With keys, each NCA's header, its
// sections and the files of its PFS0 sections.

#include <format>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "biwa/cli/run.h"
#include "biwa/core/hex.h"
#include "biwa/nx/keys.h"
#include "biwa/nx/nca.h"
#include "biwa/nx/pfs0.h"
#include "biwa/nx/section.h"
#include "biwa/nx/storage.h"
#include "biwa/nx/ticket.h"
#include "commands.h"

namespace biwa::cli {
namespace {

constexpr std::string_view kUsage = "[-k <prod.keys>] <file>";

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

void DescribeNca(const nx::Pfs0& nsp, const nx::Pfs0Entry& entry, const nx::KeySet& keys,
                 const std::map<std::string, nx::Ticket>& tickets, std::ostream& out, FirstError& first) {
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

    std::optional<nx::Aes128Key> key;
    const auto ticket = tickets.find(ToHex(header->rights_id));
    const Result<nx::Aes128Key> ctr_key =
        nx::NcaCtrKey(*header, keys, ticket == tickets.end() ? nullptr : &ticket->second);
    if (ctr_key) {
        key = *ctr_key;
    }
    for (const nx::NcaSection& section : header->sections) {
        out << "    " << SectionLine(section) << '\n';
        if (section.fs_type != nx::NcaFsType::kPartitionFs) {
            continue;
        }
        if (!ctr_key && section.encryption != nx::NcaEncryption::kNone) {
            fail(ctr_key.error(), "      ");
            continue;
        }
        Result<nx::StoragePtr> decrypted = nx::OpenNcaSection(*storage, section, key);
        if (!decrypted) {
            fail(decrypted.error(), "      ");
            continue;
        }
        Result<nx::StoragePtr> data = nx::OpenSectionData(*decrypted, section);
        if (!data) {
            fail(data.error(), "      ");
            continue;
        }
        const Result<nx::Pfs0> pfs = nx::Pfs0::Open(*data);
        if (!pfs) {
            fail(pfs.error(), "      ");
            continue;
        }
        ListPfs0(*pfs, "      ", out);
    }
}

}  // namespace

int RunInfo(Args args, std::ostream& out, std::ostream& err) {
    std::optional<std::string_view> keys_path;
    std::optional<std::string_view> file;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-k" || args[i] == "--keys") {
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
    std::map<std::string, nx::Ticket> tickets;
    for (const nx::Pfs0Entry& entry : nsp->entries()) {
        if (!entry.name.ends_with(".tik")) {
            continue;
        }
        Result<nx::StoragePtr> tik = nsp->OpenEntry(entry);
        Result<nx::Ticket> ticket = tik ? nx::ReadTicket(**tik) : Result<nx::Ticket>(std::unexpected(tik.error()));
        if (!ticket) {
            out << "  " << entry.name << ": error: " << ticket.error() << '\n';
            first.Note(ticket.error());
            continue;
        }
        tickets.emplace(ToHex(ticket->rights_id), *ticket);
    }
    for (const nx::Pfs0Entry& entry : nsp->entries()) {
        if (entry.name.ends_with(".nca")) {
            DescribeNca(*nsp, entry, *keys, tickets, out, first);
        }
    }
    return first.code ? ExitCodeFor(*first.code) : kExitOk;
}

}  // namespace biwa::cli
