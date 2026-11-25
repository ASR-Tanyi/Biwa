// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstdint>
#include <string>

#include "biwa/core/error.h"
#include "biwa/nx/storage.h"

namespace biwa::nx {

// Largest hash block Biwa accepts; real sections use 4 KiB to 64 KiB.
inline constexpr std::uint64_t kMaxHashBlockSize = std::uint64_t{64} << 20;

enum class HashPadding : std::uint8_t {
    kNone,   // a short last block is hashed as it is (HierarchicalSha256)
    kZeros,  // a short last block is hashed as if filled with zeros to the block size (IVFC)
};

// `data`, checked on every read: block i must hash (SHA-256) to bytes [32 i, 32 i + 32) of
// `hashes`. A read checks every block it touches; a block that does not match is Errc::kCorrupt,
// naming `what` and the block. Errc::kCorrupt as well if the block size is 0 or above
// kMaxHashBlockSize, or if `hashes` has too few entries for `data`.
Result<StoragePtr> MakeHashCheckedStorage(StoragePtr hashes, StoragePtr data, std::uint64_t block_size,
                                          HashPadding padding, std::string what);

}  // namespace biwa::nx
