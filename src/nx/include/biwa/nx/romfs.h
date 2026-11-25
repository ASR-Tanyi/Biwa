// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "biwa/core/error.h"
#include "biwa/nx/storage.h"

namespace biwa::nx {

struct RomfsEntry {
    std::string name;
    bool is_directory = false;
    std::uint64_t size = 0;  // files only
};

// A RomFS image: a header, directory and file tables, and file data (docs/design/03-extract.md).
// Paths use '/'; a leading '/' is optional and "" or "/" is the root. Names are checked when they
// are read: an empty name, one with '/', '\\' or a NUL byte, and "." or ".." are Errc::kCorrupt, so
// paths from a RomFS are safe to use on disk. Loops in the tables are Errc::kCorrupt as well.
class Romfs {
   public:
    // Reads the header and both entry tables (kept in memory; the file data is not).
    static Result<Romfs> Open(StoragePtr storage);

    // The directory at `path`: subdirectories, then files, each in table order.
    [[nodiscard]] Result<std::vector<RomfsEntry>> List(std::string_view path) const;

    // The bytes of the file at `path`.
    [[nodiscard]] Result<StoragePtr> OpenFile(std::string_view path) const;

    // Calls `fn` for every file, depth first in table order, with its full path ("/a/b.bin") and
    // size. Stops at the first error, from the tables or from `fn`.
    using FileFn = std::function<Status(std::string_view path, std::uint64_t size)>;
    [[nodiscard]] Status ForEachFile(const FileFn& fn) const;

   private:
    struct Dir;
    struct File;

    Romfs(StoragePtr storage, std::vector<std::byte> dirs, std::vector<std::byte> files, std::uint64_t data_offset)
        : storage_(std::move(storage)), dirs_(std::move(dirs)), files_(std::move(files)), data_offset_(data_offset) {}

    [[nodiscard]] Result<Dir> ReadDir(std::uint32_t offset) const;
    [[nodiscard]] Result<File> ReadFile(std::uint32_t offset) const;
    [[nodiscard]] std::size_t MaxSteps() const;

    struct Found;
    [[nodiscard]] Result<Found> Find(std::string_view path) const;

    StoragePtr storage_;
    std::vector<std::byte> dirs_;
    std::vector<std::byte> files_;
    std::uint64_t data_offset_ = 0;
};

}  // namespace biwa::nx
