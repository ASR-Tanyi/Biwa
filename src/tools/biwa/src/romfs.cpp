// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

// `biwa romfs ls|cat`: look inside a RomFS, either a RomFS image or the RomFS of an NSP's program
// (or add-on data) NCA.

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "biwa/cli/run.h"
#include "biwa/core/file.h"
#include "biwa/nx/content.h"
#include "biwa/nx/keys.h"
#include "biwa/nx/nca.h"
#include "biwa/nx/pfs0.h"
#include "biwa/nx/romfs.h"
#include "biwa/nx/storage.h"
#include "commands.h"

namespace biwa::cli {
namespace {

constexpr std::string_view kUsage =
    "ls [-k <prod.keys>] [-r] <file> [path]\n"
    "       biwa romfs cat [-k <prod.keys>] [-o <out>] <file> <path>";

// The RomFS of the first program or public data NCA that has one.
Result<nx::Romfs> OpenPackageRomfs(const nx::StoragePtr& storage, const nx::KeySet& keys, std::ostream& err) {
    Result<nx::Pfs0> package = nx::Pfs0::Open(storage);
    if (!package) {
        return std::unexpected(std::move(package).error());
    }
    const nx::TicketMap tickets = nx::ReadTickets(*package, [&](std::string_view name, const Error& error) {
        err << "biwa romfs: warning: " << name << ": " << error.message() << '\n';
    });
    std::optional<Error> first_error;
    for (const nx::Pfs0Entry& entry : package->entries()) {
        if (!entry.name.ends_with(".nca")) {
            continue;
        }
        Result<nx::StoragePtr> nca = package->OpenEntry(entry);
        Result<nx::NcaHeader> header = nca ? nx::ReadNcaHeader(**nca, keys)
                                           : Result<nx::NcaHeader>(std::unexpected(nca.error()));
        if (!header) {
            if (!first_error) {
                first_error = std::move(header).error().WithContext(entry.name);
            }
            continue;
        }
        if (header->content_type != nx::NcaContentType::kProgram &&
            header->content_type != nx::NcaContentType::kPublicData) {
            continue;
        }
        const auto section = std::ranges::find(header->sections, nx::NcaFsType::kRomFs, &nx::NcaSection::fs_type);
        if (section == header->sections.end()) {
            continue;
        }
        Result<nx::StoragePtr> fs = nx::OpenSectionFs(*nca, *header, *section, keys, tickets);
        if (!fs) {
            return std::unexpected(std::move(fs).error().WithContext(entry.name));
        }
        return nx::Romfs::Open(std::move(*fs));
    }
    if (first_error) {
        return std::unexpected(std::move(*first_error));
    }
    return Fail(Errc::kNotFound, "the package has no program or data NCA with a RomFS");
}

// `file` is an NSP (keys needed) or a RomFS image.
Result<nx::Romfs> OpenRomfs(const std::filesystem::path& path, const std::optional<nx::KeySet>& keys,
                            std::ostream& err) {
    Result<nx::StoragePtr> storage = nx::OpenFileStorage(path);
    if (!storage) {
        return std::unexpected(std::move(storage).error());
    }
    std::array<std::byte, 8> magic{};
    if ((*storage)->Size() >= magic.size()) {
        BIWA_RETURN_IF_ERROR((*storage)->Read(0, magic));
    }
    if (std::memcmp(magic.data(), "PFS0", 4) == 0) {
        if (!keys) {
            return Fail(Errc::kMissingKey, "an NSP needs -k <prod.keys>");
        }
        return OpenPackageRomfs(*storage, *keys, err);
    }
    std::uint64_t header_size = 0;
    std::memcpy(&header_size, magic.data(), sizeof(header_size));
    if (header_size != 0x50) {
        return Failf(Errc::kInvalidArgument, "{} is neither an NSP nor a RomFS image", PathText(path));
    }
    return nx::Romfs::Open(std::move(*storage));
}

int RunLs(const nx::Romfs& romfs, std::string_view path, bool recursive, std::ostream& out, std::ostream& err) {
    if (!recursive) {
        const Result<std::vector<nx::RomfsEntry>> entries = romfs.List(path);
        if (!entries) {
            return Report("romfs", entries.error(), err);
        }
        for (const nx::RomfsEntry& entry : *entries) {
            if (entry.is_directory) {
                out << std::format("{:>14}  {}/\n", "", entry.name);
            } else {
                out << std::format("{:>14}  {}\n", entry.size, entry.name);
            }
        }
        return kExitOk;
    }
    // Every file under `path`, with its full path.
    const Result<std::vector<nx::RomfsEntry>> top = romfs.List(path);
    if (!top) {
        return Report("romfs", top.error(), err);
    }
    std::string prefix = "/";
    for (std::size_t at = 0; at < path.size();) {
        const std::size_t slash = std::min(path.find('/', at), path.size());
        if (slash > at) {
            prefix += std::format("{}/", path.substr(at, slash - at));
        }
        at = slash + 1;
    }
    std::uint64_t count = 0;
    std::uint64_t bytes = 0;
    const Status walked = romfs.ForEachFile([&](std::string_view file, std::uint64_t size) -> Status {
        if (file.starts_with(prefix)) {
            out << std::format("{:>14}  {}\n", size, file);
            ++count;
            bytes += size;
        }
        return {};
    });
    if (!walked) {
        return Report("romfs", walked.error(), err);
    }
    out << std::format("{:>14}  bytes in {} {}\n", bytes, count, count == 1 ? "file" : "files");
    return kExitOk;
}

int RunCat(const nx::Romfs& romfs, std::string_view path, const std::optional<std::string_view>& output,
           std::ostream& out, std::ostream& err) {
    const Result<nx::StoragePtr> file = romfs.OpenFile(path);
    if (!file) {
        return Report("romfs", file.error(), err);
    }
    if (output) {
        Result<OutputFile> to = OutputFile::Create(std::filesystem::path(*output));
        Status status = to ? nx::CopyStorage(**file, *to) : Status(std::unexpected(to.error()));
        if (status) {
            status = to->Close();
        }
        return status ? kExitOk : Report("romfs", status.error(), err);
    }
    std::vector<std::byte> buffer(std::size_t{1} << 20);
    const std::uint64_t size = (*file)->Size();
    for (std::uint64_t offset = 0; offset < size;) {
        const std::span<std::byte> piece(buffer.data(),
                                         static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset)));
        const Status read = (*file)->Read(offset, piece);
        if (!read) {
            return Report("romfs", read.error(), err);
        }
        out.write(reinterpret_cast<const char*>(piece.data()), static_cast<std::streamsize>(piece.size()));
        if (!out) {
            return Report("romfs", Error(Errc::kIo, "could not write to standard output"), err);
        }
        offset += piece.size();
    }
    return kExitOk;
}

}  // namespace

int RunRomfs(Args args, std::ostream& out, std::ostream& err) {
    if (args.empty() || (args[0] != "ls" && args[0] != "cat")) {
        return UsageError("romfs", kUsage, "ls or cat is needed", err);
    }
    const bool ls = args[0] == "ls";
    std::optional<std::string_view> keys_path;
    std::optional<std::string_view> output;
    bool recursive = false;
    std::vector<std::string_view> positional;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-k" || args[i] == "--keys" || (!ls && args[i] == "-o")) {
            if (i + 1 >= args.size()) {
                return UsageError("romfs", kUsage, std::format("{} needs a file", args[i]), err);
            }
            if (args[i] == "-o") {
                output = args[++i];
            } else {
                keys_path = args[++i];
            }
        } else if (ls && args[i] == "-r") {
            recursive = true;
        } else {
            positional.push_back(args[i]);
        }
    }
    if (positional.empty() || positional.size() > 2 || (!ls && positional.size() != 2)) {
        return UsageError("romfs", kUsage, ls ? "a file and at most one path are needed" : "a file and a path are needed",
                          err);
    }

    std::optional<nx::KeySet> keys;
    if (keys_path) {
        Result<nx::KeySet> loaded = nx::KeySet::Load(std::filesystem::path(*keys_path));
        if (!loaded) {
            return Report("romfs", loaded.error(), err);
        }
        keys = std::move(*loaded);
    }
    const std::filesystem::path file(positional[0]);
    const Result<nx::Romfs> romfs = OpenRomfs(file, keys, err);
    if (!romfs) {
        return Report("romfs", romfs.error(), err);
    }
    const std::string_view path = positional.size() == 2 ? positional[1] : std::string_view("/");
    return ls ? RunLs(*romfs, path, recursive, out, err) : RunCat(*romfs, path, output, out, err);
}

}  // namespace biwa::cli
