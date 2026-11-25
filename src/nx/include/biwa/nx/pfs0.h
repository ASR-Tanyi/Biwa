// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "biwa/core/error.h"
#include "biwa/nx/storage.h"

namespace biwa::nx {

// One file in a PFS0 container.
struct Pfs0Entry {
    std::string name;
    std::uint64_t offset = 0;  // from the start of the container
    std::uint64_t size = 0;
};

// PFS0 ("partition file system"): the container of an NSP and of an ExeFS.
//
//   0x00  "PFS0", file count (u32), string table size (u32), reserved (u32)
//   0x10  file count x { data offset (u64), size (u64), name offset (u32), reserved (u32) }
//         string table (NUL-terminated names)
//         file data; data offsets count from here
class Pfs0 {
   public:
    // Errc::kInvalidArgument if the storage is not a PFS0 at all (wrong magic), Errc::kCorrupt if
    // it is one but its tables are damaged.
    static Result<Pfs0> Open(StoragePtr storage);

    [[nodiscard]] std::span<const Pfs0Entry> entries() const { return entries_; }

    // nullptr if there is no such file. Names are compared exactly.
    [[nodiscard]] const Pfs0Entry* Find(std::string_view name) const;

    // The file's bytes, as a window of the container.
    [[nodiscard]] Result<StoragePtr> OpenEntry(const Pfs0Entry& entry) const;

   private:
    Pfs0(StoragePtr storage, std::vector<Pfs0Entry> entries)
        : storage_(std::move(storage)), entries_(std::move(entries)) {}

    StoragePtr storage_;
    std::vector<Pfs0Entry> entries_;
};

}  // namespace biwa::nx
